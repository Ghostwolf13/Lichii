#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What the pipeline records of a shader cache v2 log froze at capture.

  cache_records.py dump <lichii-cache.v2.bin> [--shader HASH] [--unbound]
  cache_records.py diff <carried lichii-cache.v2.bin> <cold lichii-cache.v2.bin> [--shader HASH]

A record holds guest environments, not SPIR-V: every boot translates and emits it again with the
running build. What it keeps from the session that captured it are the answers the guest gave the
translator then, among them whether each constant buffer word it loads a pointer from held a
mapped address. A slot that was unbound at capture stays unbound in the translation, so its loads
and stores are dropped, until the pipeline is translated again from the game. Only native compute
slots whose two words were zero are watched for that (#551, PipelineCache::StaleNullPointerSlot).

dump prints one JSON object per live graphics or compute record: its stages, shader hashes, the
host read properties it was made with, and every unbound pointer slot with the words captured for
it (for a table entry, the slot that points to its table) and whether anything translates it again
("rebuilt"). diff prints the shader hashes whose
pointer classifications differ between two caches of one title, such as a carried cache that shows
a bug and a cold one that doesn't.

Output holds hashes, slots and guest addresses only, never shader code. The caches hold the game's
shaders: keep them private. Standard library only; record header CRCs are checked, payload hashes
(XXH3) aren't.
"""

import argparse
import json
import struct
import sys
import zlib

FILE_MAGIC = b"LXSHCACH"
FORMAT_VERSION = 2
FILE_HEADER_SIZE = 32
RECORD_MAGIC = 0x5243584C
RECORD_HEADER_SIZE = 24
RECORD_HASH_SIZE = 8
MAX_PAYLOAD_SIZE = 256 << 20

KIND_GRAPHICS = 2
KIND_COMPUTE = 3
KIND_TOMBSTONE = 4

STAGES = {0: "vertex_b", 1: "tess_control", 2: "tess_eval", 3: "geometry", 4: "fragment",
          5: "compute", 6: "vertex_a", 7: "task", 8: "mesh"}
STAGE_GEOMETRY = 3
STAGE_COMPUTE = 5
SPH_SIZE = 80
MAX_RECORDED_VALUES = 1 << 16
READ_PROPERTIES_KEY = 0xFFFFFFFF << 32
NULL_TEXTURE_2D_KEY = (0xFFFFFFFF << 32) | 1


class Damaged(Exception):
    pass


def parse_log(data):
    """The live graphics and compute records of a v2 log, as (kind, key, payload) in file order."""
    if data[:8] == FILE_MAGIC and len(data) >= FILE_HEADER_SIZE and \
            struct.unpack_from("<I", data, 24)[0] == zlib.crc32(data[:24]):
        version = struct.unpack_from("<I", data, 8)[0]
        if version != FORMAT_VERSION:
            raise Damaged(f"format version {version}, not {FORMAT_VERSION}")
    live = {}
    order = 0
    offset = FILE_HEADER_SIZE
    while offset + RECORD_HEADER_SIZE <= len(data):
        magic, kind, _flags, size, key, crc = struct.unpack_from("<IHHIQI", data, offset)
        end = offset + RECORD_HEADER_SIZE + size
        if (magic != RECORD_MAGIC or crc != zlib.crc32(data[offset:offset + 20]) or kind == 0 or
                size > MAX_PAYLOAD_SIZE or end + RECORD_HASH_SIZE > len(data)):
            offset += 8
            continue
        payload = data[offset + RECORD_HEADER_SIZE:end]
        if kind == KIND_TOMBSTONE:
            if len(payload) >= 2:
                live.pop((struct.unpack_from("<H", payload)[0], key), None)
        elif kind in (KIND_GRAPHICS, KIND_COMPUTE):
            live[(kind, key)] = (order, payload)
            order += 1
        offset = (end + RECORD_HASH_SIZE + 7) // 8 * 8
    return [(kind, key, payload)
            for (kind, key), (_, payload) in sorted(live.items(), key=lambda item: item[1][0])]


class Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def take(self, size):
        if self.offset + size > len(self.data):
            raise Damaged("environment past the end of its record")
        self.offset += size
        return self.data[self.offset - size:self.offset]

    def unpack(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))

    def pairs(self, count, fmt):
        if count > MAX_RECORDED_VALUES:
            raise Damaged("too many values in an environment")
        return [self.unpack(fmt) for _ in range(count)]


def read_environment(reader):
    """One environment as GenericEnvironment::Serialize writes it, its code skipped."""
    (code_size, num_texture_types, num_pixel_formats, num_cbuf_values, num_pointer_types,
     num_replacements) = reader.unpack("<6Q")
    _local_memory, texture_bound, _start, _lowest, _highest, _viewport, stage = reader.unpack("<7I")
    if stage not in STAGES:
        raise Damaged(f"invalid stage {stage}")
    reader.take(code_size)
    reader.pairs(num_texture_types, "<II")
    reader.pairs(num_pixel_formats, "<Ii")
    cbuf_values = dict(reader.pairs(num_cbuf_values, "<QI"))
    pointer_types = dict(reader.pairs(num_pointer_types, "<QB"))
    reader.pairs(num_replacements, "<QI")
    if stage == STAGE_COMPUTE:
        reader.take(16)
    else:
        reader.take(SPH_SIZE)
        if stage == STAGE_GEOMETRY:
            reader.take(32)
        if STAGES[stage] in ("task", "mesh"):
            reader.take(12 + SPH_SIZE)
    return {
        "stage": stage,
        "texture_bound": texture_bound,
        "read_properties": cbuf_values.pop(READ_PROPERTIES_KEY, None),
        "null_texture_2d": cbuf_values.pop(NULL_TEXTURE_2D_KEY, None) is not None,
        "cbuf_values": cbuf_values,
        "pointer_types": pointer_types,
    }


def slot_name(key):
    index = (key >> 32) & 0xFF
    offset = key & 0xFFFFFFFF
    if key >> 63:
        return f"c{index}[{offset:#x}][{(key >> 40) & 0x3FFFFF:#x}]"
    return f"c{index}[{offset:#x}]"


def unbound_slots(environment, compute):
    """Every pointer slot the capture saw unbound, and whether the running build rebuilds it."""
    slots = []
    values = environment["cbuf_values"]
    for key, mapped in sorted(environment["pointer_types"].items()):
        if mapped:
            continue
        direct = key & ((1 << 40) - 1)
        low, high = values.get(direct), values.get(direct + 4)
        address = None if low is None or high is None else low | (high << 32)
        if key >> 63:
            kind = "table"
        elif address == 0:
            kind = "null"
        else:
            kind = "unmapped"
        slots.append({
            "slot": slot_name(key),
            "kind": kind,
            "captured": None if address is None else f"{address:#x}",
            "rebuilt": compute and kind == "null",
        })
    return slots


def read_record(kind, key, payload):
    reader = Reader(payload)
    (count,) = reader.unpack("<I")
    if count == 0 or count > 6:
        raise Damaged(f"{count} environments")
    environments = [read_environment(reader) for _ in range(count)]
    compute = kind == KIND_COMPUTE
    key_bytes = payload[reader.offset:]
    if compute:
        if len(key_bytes) != 24 or len(environments) != 1:
            raise Damaged("not a compute record")
        hashes = [struct.unpack_from("<Q", key_bytes)[0]]
    else:
        if len(key_bytes) < 48:
            raise Damaged("graphics key cut short")
        hashes = [value for value in struct.unpack_from("<6Q", key_bytes) if value]
        if len(hashes) != len(environments):
            raise Damaged("graphics key names another number of stages")
    return {
        "kind": "compute" if compute else "graphics",
        "record": f"{key:016x}",
        "stages": [STAGES[environment["stage"]] for environment in environments],
        "shaders": [f"{value:016x}" for value in hashes],
        "read_properties": sorted({environment["read_properties"] for environment in environments
                                   if environment["read_properties"] is not None}),
        "environments": environments,
        "unbound": [dict(slot, stage=STAGES[environment["stage"]], shader=f"{shader:016x}")
                    for environment, shader in zip(environments, hashes)
                    for slot in unbound_slots(environment, compute)],
    }


def read_cache(path):
    with open(path, "rb") as file:
        data = file.read()
    records, damaged = [], 0
    for kind, key, payload in parse_log(data):
        try:
            records.append(read_record(kind, key, payload))
        except (Damaged, struct.error):
            damaged += 1
    return records, damaged


def classifications(records, shader=None):
    """Per shader hash and stage, what every pointer slot was classified as over all its records."""
    result = {}
    for record in records:
        for environment, shader_hash in zip(record["environments"], record["shaders"]):
            if shader and shader_hash != shader:
                continue
            slots = result.setdefault((shader_hash, STAGES[environment["stage"]]), {})
            for key, mapped in environment["pointer_types"].items():
                slots.setdefault(slot_name(key), set()).add("mapped" if mapped else "unbound")
    return result


def command_dump(args):
    records, damaged = read_cache(args.cache)
    for record in records:
        if args.shader and args.shader not in record["shaders"]:
            continue
        if args.unbound and not record["unbound"]:
            continue
        print(json.dumps({name: value for name, value in record.items() if name != "environments"}))
    print(json.dumps({"records": len(records), "damaged": damaged}), file=sys.stderr)
    return 0


def command_diff(args):
    carried_records, _ = read_cache(args.carried)
    cold_records, _ = read_cache(args.cold)
    carried = classifications(carried_records, args.shader)
    cold = classifications(cold_records, args.shader)
    shared = carried.keys() & cold.keys()
    differ = 0
    for shader, stage in sorted(shared):
        a, b = carried[(shader, stage)], cold[(shader, stage)]
        slots = [{"slot": slot, "carried": sorted(a.get(slot, ())), "cold": sorted(b.get(slot, ()))}
                 for slot in sorted(a.keys() | b.keys()) if a.get(slot) != b.get(slot)]
        if slots:
            differ += 1
            print(json.dumps({"shader": shader, "stage": stage, "slots": slots}))
    print(json.dumps({"carried_records": len(carried_records), "cold_records": len(cold_records),
                      "shared_shaders": len(shared), "differ": differ}), file=sys.stderr)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    dump = commands.add_parser("dump")
    dump.add_argument("cache")
    dump.add_argument("--shader", type=str.lower)
    dump.add_argument("--unbound", action="store_true", help="only records with unbound slots")
    dump.set_defaults(run=command_dump)
    diff = commands.add_parser("diff")
    diff.add_argument("carried")
    diff.add_argument("cold")
    diff.add_argument("--shader", type=str.lower)
    diff.set_defaults(run=command_diff)
    args = parser.parse_args(argv)
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
