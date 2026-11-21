#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import io
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cache_records  # noqa: E402

READ_PROPERTIES = 0x15
FOLIAGE = 0x59C1699F529E7C08
TERRAIN = 0x1111222233334444


def cbuf_key(index, offset):
    return (index << 32) | offset


def table_key(index, offset, indirect):
    return (1 << 63) | (indirect << 40) | cbuf_key(index, offset)


def environment(stage, cbuf_values=(), pointer_types=()):
    """An environment laid out as GenericEnvironment::Serialize writes it."""
    code = struct.pack("<2Q", 0x123456789ABCDEF0, 0xFEDCBA9876543210)
    values = list(cbuf_values) + [(cache_records.READ_PROPERTIES_KEY, READ_PROPERTIES),
                                  (cache_records.NULL_TEXTURE_2D_KEY, 1)]
    out = struct.pack("<6Q", len(code), 1, 0, len(values), len(pointer_types), 0)
    out += struct.pack("<7I", 0, 2, 0, 0, 8, 1, stage)
    out += code
    out += struct.pack("<II", 3, 2)
    out += b"".join(struct.pack("<QI", key, value) for key, value in values)
    out += b"".join(struct.pack("<QB", key, mapped) for key, mapped in pointer_types)
    if stage == cache_records.STAGE_COMPUTE:
        out += struct.pack("<3II", 64, 1, 1, 0)
    else:
        out += bytes(cache_records.SPH_SIZE)
    return out


def compute_payload(shader, cbuf_values=(), pointer_types=()):
    env = environment(cache_records.STAGE_COMPUTE, cbuf_values, pointer_types)
    return struct.pack("<I", 1) + env + struct.pack("<QI3I", shader, 0, 64, 1, 1)


def graphics_payload(vertex, fragment, pointer_types=()):
    envs = environment(0) + environment(4, [(cbuf_key(3, 0x10), 0), (cbuf_key(3, 0x14), 0)],
                                        pointer_types)
    key = struct.pack("<6Q", 0, vertex, 0, 0, 0, fragment) + bytes(40)
    return struct.pack("<I", 2) + envs + key


def record(kind, key, payload):
    header = struct.pack("<IHHIQ", cache_records.RECORD_MAGIC, kind, 0, len(payload), key)
    out = header + struct.pack("<I", zlib.crc32(header)) + payload + bytes(8)
    return out + bytes(-len(out) % 8)


def log(*records):
    header = cache_records.FILE_MAGIC + struct.pack("<IIQ", 2, 32, 0)
    return header + struct.pack("<II", zlib.crc32(header), 0) + b"".join(records)


def foliage(slot_words, mapped):
    """OoT's attack culling compute: its work buffer at c4[0xd0], with these words captured."""
    low, high = slot_words & 0xFFFFFFFF, slot_words >> 32
    return compute_payload(FOLIAGE, [(cbuf_key(4, 0xD0), low), (cbuf_key(4, 0xD4), high)],
                           [(cbuf_key(4, 0xD0), int(mapped))])


class CacheRecordsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.dir.cleanup()

    def write(self, name, data):
        path = os.path.join(self.dir.name, name)
        with open(path, "wb") as file:
            file.write(data)
        return path

    def run_tool(self, *args):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(io.StringIO()):
            self.assertEqual(cache_records.main(list(args)), 0)
        return [json.loads(line) for line in out.getvalue().splitlines()]

    def test_dump_names_slots_nothing_rebuilds(self):
        path = self.write("carried.bin", log(
            record(3, 1, foliage(0x7F00001000, mapped=False)),
            record(3, 2, compute_payload(TERRAIN, [(cbuf_key(0, 0x20), 0), (cbuf_key(0, 0x24), 0)],
                                         [(cbuf_key(0, 0x20), 0)])),
            record(3, 3, compute_payload(TERRAIN + 1, [(cbuf_key(1, 0), 0x1000), (cbuf_key(1, 4), 0)],
                                         [(cbuf_key(1, 0), 1), (table_key(1, 0, 0x18), 0)])),
            record(2, 4, graphics_payload(TERRAIN + 2, TERRAIN + 3, [(cbuf_key(3, 0x10), 0)])),
        ))
        records = {record["shaders"][-1]: record for record in self.run_tool("dump", path)}
        self.assertEqual(records[f"{FOLIAGE:016x}"]["unbound"], [{
            "slot": "c4[0xd0]", "kind": "unmapped", "captured": "0x7f00001000", "rebuilt": False,
            "stage": "compute", "shader": f"{FOLIAGE:016x}"}])
        self.assertEqual(records[f"{FOLIAGE:016x}"]["read_properties"], [READ_PROPERTIES])
        self.assertEqual(records[f"{TERRAIN:016x}"]["unbound"][0]["kind"], "null")
        self.assertTrue(records[f"{TERRAIN:016x}"]["unbound"][0]["rebuilt"])
        table = records[f"{TERRAIN + 1:016x}"]["unbound"]
        self.assertEqual([(slot["slot"], slot["kind"], slot["rebuilt"]) for slot in table],
                         [("c1[0x0][0x18]", "table", False)])
        graphics = records[f"{TERRAIN + 3:016x}"]
        self.assertEqual(graphics["stages"], ["vertex_b", "fragment"])
        self.assertEqual(graphics["shaders"], [f"{TERRAIN + 2:016x}", f"{TERRAIN + 3:016x}"])
        self.assertEqual([(slot["stage"], slot["kind"], slot["rebuilt"])
                          for slot in graphics["unbound"]], [("fragment", "null", False)])
        self.assertEqual(len(self.run_tool("dump", path, "--shader", f"{FOLIAGE:016X}")), 1)
        self.assertEqual(len(self.run_tool("dump", path, "--unbound")), 4)

    def test_later_records_and_tombstones_win(self):
        path = self.write("log.bin", log(
            record(3, 1, foliage(0, mapped=False)),
            record(3, 1, foliage(0x7F00001000, mapped=True)),
            record(3, 2, compute_payload(TERRAIN)),
            record(4, 2, struct.pack("<H", 3)),
            b"\xffdamaged",
        ))
        records = self.run_tool("dump", path)
        self.assertEqual([record["shaders"] for record in records], [[f"{FOLIAGE:016x}"]])
        self.assertEqual(records[0]["unbound"], [])

    def test_diff_names_the_record_that_froze_another_state(self):
        carried = self.write("carried.bin", log(
            record(3, 1, foliage(0x7F00001000, mapped=False)),
            record(3, 2, compute_payload(TERRAIN, pointer_types=[(cbuf_key(0, 0x20), 1)])),
        ))
        cold = self.write("cold.bin", log(
            record(3, 2, compute_payload(TERRAIN, pointer_types=[(cbuf_key(0, 0x20), 1)])),
            record(3, 1, foliage(0x7F00001000, mapped=True)),
        ))
        self.assertEqual(self.run_tool("diff", carried, cold), [{
            "shader": f"{FOLIAGE:016x}", "stage": "compute",
            "slots": [{"slot": "c4[0xd0]", "carried": ["unbound"], "cold": ["mapped"]}]}])
        self.assertEqual(self.run_tool("diff", cold, cold), [])


if __name__ == "__main__":
    unittest.main()
