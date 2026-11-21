#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shader cache and stutter metrics from Lichii logs.

  cache_metrics.py parse <log> [--session N] [--start-s S] [--end-s S]
  cache_metrics.py compare --a <log|json>... --b <log|json>... [--label-a A] [--label-b B]

parse prints one JSON object per log. The numbers come from the EDEN_NX2_STUTTER_TRACE=1 lines
(Nx2Boot, Nx2Stutter, Nx2StutterSummary). Logs from builds without them fall back to the existing
lines (Total Pipeline Count, Shader precache, Runtime pipelines in the last 10 s, DKB renderer),
and every metric names its source. compare prints a markdown table of the mean, min and max of
repeated runs of A and B and the difference of the means.

Standard library only.
"""

import argparse
import json
import math
import re
import statistics
import sys

TIME = re.compile(r"^\[\s*(\d+\.\d+)\]")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
FIELD = re.compile(r"(\w+)=(\S+)")
SESSION_START = re.compile(r"Booting game: ([0-9A-Fa-f]{16})")
LOADING = re.compile(r"Loading .* \(([0-9A-Fa-f]{16})\) \.\.\.")
BUILD = re.compile(r"Lichii Version: (.*)$")
PRECACHE_END = re.compile(r"Shader precache: (finished|skipped|stopped), (\d+) built, .* ([\d.]+) s$")
TOTAL_PIPELINES = re.compile(r"Total Pipeline Count: (\d+)")
RUNTIME_10S = re.compile(r"Runtime pipelines in the last 10 s: (\d+) built, ([\d.]+) ms on the GPU thread")
RUNTIME_ONE = re.compile(r"(Graphics|Compute) pipeline built on the GPU thread in ([\d.]+) ms")
# Nx2StutterSummary histograms: 1 ms bins, the last holding every longer frame
OVERFLOW_BIN_MS = 299
DKB_FRAME = re.compile(r"DKB renderer .*cpu_frame_wall_ms=([\d.]+)")
DRIVER_BUILD_TIMES = re.compile(
    r"(Shader cache v2 boot load|Background prebuild done).*driver build times: (\d+) under 10 ms, "
    r"(\d+) 10-100 ms, (\d+) over 100 ms \(([\d.]+) s in all, longest ([\d.]+) ms\)")

FRAME_MS_60 = 1000.0 / 60.0
STUTTER_MS = 33.0
CATEGORIES = ("gfx_miss", "compute_miss", "pipeline_wait", "shader_capture", "texture", "buffer",
              "query_fence", "present", "other")
# What the phase 0 question needs: is a stutter a shader stutter or not.
CAUSE_GROUPS = {
    "gfx_miss": "translation",
    "compute_miss": "translation",
    "shader_capture": "translation",
    "pipeline_wait": "pipeline_build",
    "texture": "texture_cache",
}
GROUPS = ("translation", "pipeline_build", "texture_cache", "other")


class Line:
    __slots__ = ("t", "text")

    def __init__(self, t, text):
        self.t = t
        self.text = text


def fields(text):
    """key=value pairs of a trace line, numbers converted."""
    result = {}
    for key, value in FIELD.findall(text):
        try:
            result[key] = float(value) if "." in value else int(value)
        except ValueError:
            result[key] = value
    return result


def pairs(value):
    """'a:1,b:2.5' -> {'a': 1.0, 'b': 2.5}; 'none' -> {}."""
    if not isinstance(value, str) or value in ("", "none"):
        return {}
    out = {}
    for item in value.split(","):
        key, _, number = item.rpartition(":")
        out[key] = float(number)
    return out


def read_lines(path):
    lines = []
    last = 0.0
    with open(path, encoding="utf-8", errors="replace") as stream:
        for raw in stream:
            text = ANSI.sub("", raw).replace("\x00", "").rstrip("\r\n")
            match = TIME.match(text)
            if match:
                last = float(match.group(1))
            lines.append(Line(last, text))
    return lines


def split_sessions(lines):
    """Each 'Booting game' starts a session; without any, each boot_start marker; else one."""
    starts = [i for i, line in enumerate(lines) if SESSION_START.search(line.text)]
    if not starts:
        starts = [i for i, line in enumerate(lines) if "Nx2Boot event=boot_start" in line.text]
    if not starts:
        return [lines] if lines else []
    sessions = [lines[begin:end] for begin, end in zip(starts, starts[1:] + [len(lines)])]
    # The lines before the first boot (version, settings) belong to it.
    sessions[0] = lines[:starts[0]] + sessions[0]
    return sessions


def percentile_from_histogram(histogram, fraction):
    """Midpoint of the 1 ms bin holding the given fraction of frames (resolution 1 ms); None when
    that is the last bin, which holds every frame of OVERFLOW_BIN_MS and longer."""
    total = sum(histogram.values())
    if not total:
        return None
    rank = fraction * total
    seen = 0
    for bin_ms in sorted(histogram):
        seen += histogram[bin_ms]
        if seen >= rank:
            break
    return None if bin_ms >= OVERFLOW_BIN_MS else bin_ms + 0.5


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def round_or_none(value, digits=3):
    return None if value is None else round(value, digits)


def analyze(lines, start_s=None, end_s=None):
    """Metrics of one session. start_s/end_s (log seconds) bound the frame and stutter stats;
    by default they run from the first presented frame to the end of the log."""
    markers = {}
    build = None
    program_id = None
    total_pipelines = None
    precache_line = None
    driver_loaded_t = None
    driver_saved_t = []
    runtime_10s = []
    runtime_lines = {"Graphics": [], "Compute": []}
    dkb_frames = []
    driver_build_times = {"boot": None, "prebuild": None}
    summaries = []
    stutters = []
    for line in lines:
        text = line.text
        if "Nx2" in text:
            if "Nx2Boot event=" in text:
                values = fields(text[text.index("Nx2Boot"):])
                values["t"] = line.t
                markers.setdefault(values["event"], values)
                continue
            # A line the emulator was killed while writing lacks its last field: skipped.
            if "Nx2StutterSummary " in text:
                values = fields(text[text.index("Nx2StutterSummary"):])
                values["t"] = line.t
                if "cache_bytes" in values:
                    summaries.append(values)
                continue
            if "Nx2Stutter " in text:
                values = fields(text[text.index("Nx2Stutter "):])
                values["t"] = line.t
                if "builds" in values:
                    stutters.append(values)
                continue
        if build is None and (match := BUILD.search(text)):
            build = match.group(1).strip()
        elif program_id is None and (match := SESSION_START.search(text) or LOADING.search(text)):
            program_id = match.group(1).upper()
        elif total_pipelines is None and (match := TOTAL_PIPELINES.search(text)):
            total_pipelines = (line.t, int(match.group(1)))
        elif match := PRECACHE_END.search(text):
            precache_line = (line.t, match.group(1), int(match.group(2)), float(match.group(3)))
        elif match := RUNTIME_10S.search(text):
            runtime_10s.append((line.t, int(match.group(1)), float(match.group(2))))
        elif match := RUNTIME_ONE.search(text):
            runtime_lines[match.group(1)].append((line.t, float(match.group(2))))
        elif match := DKB_FRAME.search(text):
            dkb_frames.append((line.t, float(match.group(1))))
        elif match := DRIVER_BUILD_TIMES.search(text):
            phase = "boot" if match.group(1).startswith("Shader cache") else "prebuild"
            driver_build_times[phase] = {
                "under_10ms": int(match.group(2)), "10_100ms": int(match.group(3)),
                "over_100ms": int(match.group(4)), "total_s": float(match.group(5)),
                "longest_ms": float(match.group(6))}
        elif driver_loaded_t is None and "Loaded Vulkan driver pipeline cache:" in text:
            driver_loaded_t = line.t
        elif "Vulkan driver pipelines cached at:" in text:
            driver_saved_t.append(line.t)

    def marker_t(name):
        return markers[name]["t"] if name in markers else None

    session_start_t = lines[0].t if lines else None
    for line in lines:
        if SESSION_START.search(line.text) or LOADING.search(line.text):
            session_start_t = line.t
            break
    boot_t = marker_t("boot_start")
    first_frame_t = marker_t("first_frame")
    result = {
        "build": build,
        "program_id": program_id,
        "trace": bool(markers or summaries),
        "boot_to_first_frame_s": None,
        "loading_shaders_s": None,
        "precache_s": None,
        "precache_peak_commit_mib": None,
        "memory": {},
        "records_loaded": None,
        "pipelines_queued_at_boot": total_pipelines[1] if total_pipelines else None,
        "runtime_builds": {"graphics": None, "compute": None, "total": None},
        "runtime_build_gpu_thread_ms": None,
        "frame_ms": {"p50": None, "p99": None, "frames": None},
        "frames_over_33ms": None,
        "frames_over_100ms": None,
        "stutter": None,
        "cache": {"hit_rate": None, "graphics_hit_rate": None, "compute_hit_rate": None,
                  "lookups": None, "file_bytes_at_boot": None, "file_bytes_at_end": None,
                  "driver_file_bytes_at_boot": None},
        # Wall time of each driver build (vkCreate*Pipelines) at boot and in the background
        # prebuild: a pipeline the driver's own cache held builds in a few milliseconds
        "driver_build_times": driver_build_times,
        "window_s": None,
        "sources": {},
    }
    sources = result["sources"]

    if boot_t is not None and first_frame_t is not None:
        result["boot_to_first_frame_s"] = round(first_frame_t - boot_t, 3)
        sources["boot_to_first_frame_s"] = "Nx2Boot boot_start..first_frame"

    if "shader_load_end" in markers:
        end = markers["shader_load_end"]
        if isinstance(end.get("seconds"), (int, float)):
            result["loading_shaders_s"] = round(float(end["seconds"]), 3)
        elif "shader_load_begin" in markers:
            result["loading_shaders_s"] = round(end["t"] - markers["shader_load_begin"]["t"], 3)
        sources["loading_shaders_s"] = "Nx2Boot shader_load_end"
        result["records_loaded"] = {"graphics": end.get("graphics_records"),
                                    "compute": end.get("compute_records")}
        result["cache"]["file_bytes_at_boot"] = end.get("cache_bytes")
        result["cache"]["driver_file_bytes_at_boot"] = end.get("driver_cache_bytes")
    else:
        begin = driver_loaded_t if driver_loaded_t is not None else (
            total_pipelines[0] if total_pipelines else None)
        end = next((t for t in driver_saved_t if begin is not None and t >= begin), None)
        if begin is not None and end is not None:
            result["loading_shaders_s"] = round(end - begin, 3)
            sources["loading_shaders_s"] = ("fallback: 'Loaded Vulkan driver pipeline cache' or "
                                            "'Total Pipeline Count' to 'Vulkan driver pipelines "
                                            "cached at' (misses the record read)")

    if "precache_end" in markers:
        end = markers["precache_end"]
        result["precache_s"] = end.get("seconds")
        sources["precache_s"] = "Nx2Boot precache_end"
        peak = end.get("peak_commit_mib", end.get("peak_rss_mib"))
        result["precache_peak_commit_mib"] = peak
        sources["precache_peak_commit_mib"] = (
            "Nx2Boot precache_end peak_commit_mib (process peak so far)"
            if "peak_commit_mib" in end else "Nx2Boot precache_end peak_rss_mib (Linux resident peak)")
    elif precache_line:
        result["precache_s"] = precache_line[3]
        sources["precache_s"] = "fallback: 'Shader precache: <state>, N built, ..., S s'"

    for name, values in markers.items():
        memory = {key: values[key] for key in ("commit_mib", "peak_commit_mib", "rss_mib",
                                                "peak_rss_mib") if key in values}
        if memory:
            result["memory"][name] = memory

    # The measured window for frames and stutters.
    log_end = lines[-1].t if lines else 0.0
    window_start = start_s if start_s is not None else (
        first_frame_t if first_frame_t is not None else session_start_t or 0.0)
    window_end = end_s if end_s is not None else log_end
    result["window_s"] = {"start": window_start, "end": window_end}

    def summary_in_window(summary):
        middle = summary["t"] - float(summary.get("window_s", 10.0)) / 2
        return window_start <= middle <= window_end

    window_summaries = [s for s in summaries if summary_in_window(s)]
    if summaries:
        histogram = {}
        for summary in window_summaries:
            for bin_ms, count in pairs(summary.get("hist")).items():
                histogram[int(float(bin_ms))] = histogram.get(int(float(bin_ms)), 0) + int(count)
        result["frame_ms"] = {"p50": percentile_from_histogram(histogram, 0.50),
                              "p99": percentile_from_histogram(histogram, 0.99),
                              "frames": sum(histogram.values())}
        sources["frame_ms"] = "Nx2StutterSummary hist (1 ms bins, 10 s windows whose middle is in the window)"

        builds = {"gfx": 0.0, "compute": 0.0}
        categories = {}
        lookups = {}
        driver_builds = {}
        for summary in window_summaries:
            for key, value in pairs(summary.get("builds")).items():
                builds[key] = builds.get(key, 0.0) + value
            for key, value in pairs(summary.get("cat_ms")).items():
                categories[key] = categories.get(key, 0.0) + value
            for key, value in pairs(summary.get("lookups")).items():
                lookups[key] = lookups.get(key, 0) + int(value)
            for key, value in pairs(summary.get("driver_builds")).items():
                driver_builds[key] = driver_builds.get(key, 0) + int(value)
        result["runtime_builds"] = {"graphics": int(builds["gfx"]), "compute": int(builds["compute"]),
                                    "total": int(builds["gfx"] + builds["compute"])}
        result["runtime_build_gpu_thread_ms"] = round(
            categories.get("gfx_miss", 0.0) + categories.get("compute_miss", 0.0), 1)
        result["gpu_thread_ms_by_category"] = {key: round(value, 1) for key, value in categories.items()}
        sources["runtime_builds"] = "Nx2StutterSummary builds (after the first frame, in the window)"
        sources["runtime_build_gpu_thread_ms"] = "Nx2StutterSummary cat_ms gfx_miss + compute_miss"
        graphics_hits = lookups.get("gfx_prebuilt", 0) + lookups.get("gfx_background", 0)
        graphics_misses = lookups.get("gfx_recorded", 0) + lookups.get("gfx_new", 0)
        compute_hits = lookups.get("compute_prebuilt", 0)
        compute_misses = lookups.get("compute_new", 0)

        def rate(hits, misses):
            return round(hits / (hits + misses), 4) if hits + misses else None

        result["cache"].update({
            "lookups": lookups,
            "graphics_hit_rate": rate(graphics_hits, graphics_misses),
            "compute_hit_rate": rate(compute_hits, compute_misses),
            "hit_rate": rate(graphics_hits + compute_hits, graphics_misses + compute_misses),
            # Shader cache v2's driver layer: pipelines built from stored binaries, compiled, or
            # whose stored binaries were rejected (none without it)
            "driver_builds": driver_builds,
        })
        sources["cache.hit_rate"] = ("Nx2StutterSummary lookups: first use of each pipeline in the "
                                     "session, hits = built from the disk cache before use")
        last_bytes = next((s.get("cache_bytes") for s in reversed(summaries)
                           if isinstance(s.get("cache_bytes"), int)), None)
        result["cache"]["file_bytes_at_end"] = last_bytes
    else:
        in_window = [(t, ms) for t, ms in dkb_frames if window_start <= t <= window_end]
        if in_window:
            values = [ms for _, ms in in_window]
            result["frame_ms"] = {"p50": round_or_none(percentile(values, 0.50)),
                                  "p99": round_or_none(percentile(values, 0.99)),
                                  "frames": len(values)}
            sources["frame_ms"] = "fallback: DKB renderer cpu_frame_wall_ms, one sample per second"
        window_10s = [(t, n, ms) for t, n, ms in runtime_10s if window_start <= t <= window_end + 10]
        if window_10s:
            result["runtime_builds"]["total"] = sum(n for _, n, _ in window_10s)
            result["runtime_build_gpu_thread_ms"] = round(sum(ms for _, _, ms in window_10s), 1)
            sources["runtime_builds"] = "fallback: 'Runtime pipelines in the last 10 s' (10 s windows)"
            sources["runtime_build_gpu_thread_ms"] = sources["runtime_builds"]
        for kind, key in (("Graphics", "graphics"), ("Compute", "compute")):
            logged = [ms for t, ms in runtime_lines[kind] if window_start <= t <= window_end]
            if logged:
                result["runtime_builds"][key] = len(logged)
        if any(runtime_lines.values()):
            sources["runtime_builds.graphics/compute"] = ("fallback: '<Kind> pipeline built on the GPU "
                                                          "thread' lines, only those over 2 ms (first "
                                                          "400) or 100 ms")

    # Every frame over 33 ms has its own line: exact counts and causes in the window.
    if summaries or stutters:
        in_window = [s for s in stutters if window_start <= s["t"] <= window_end]
        result["frames_over_33ms"] = sum(1 for s in in_window if float(s["ms"]) > STUTTER_MS)
        result["frames_over_100ms"] = sum(1 for s in in_window if float(s["ms"]) > 100.0)
        sources["frames_over_33ms"] = "Nx2Stutter lines in the window"
        by_cause = {}
        by_group = {group: {"frames": 0, "ms": 0.0, "excess_ms": 0.0} for group in GROUPS}
        for stutter in in_window:
            ms = float(stutter["ms"])
            cause = str(stutter.get("cause", "other"))
            excess = max(ms - FRAME_MS_60, 0.0)
            entry = by_cause.setdefault(cause, {"frames": 0, "ms": 0.0, "excess_ms": 0.0})
            group = by_group[CAUSE_GROUPS.get(cause, "other")]
            for target in (entry, group):
                target["frames"] += 1
                target["ms"] += ms
                target["excess_ms"] += excess
        for table in (by_cause, by_group):
            for entry in table.values():
                entry["ms"] = round(entry["ms"], 1)
                entry["excess_ms"] = round(entry["excess_ms"], 1)
        result["stutter"] = {"frames": len(in_window), "by_group": by_group, "by_cause": by_cause}
        sources["stutter"] = ("Nx2Stutter cause (top category of the frame); excess_ms is the "
                              "interval past 16.7 ms")
    return result


def parse_log(path, session=None, start_s=None, end_s=None):
    sessions = split_sessions(read_lines(path))
    if not sessions:
        raise ValueError(f"{path}: empty log")
    index = len(sessions) - 1 if session is None else session
    result = analyze(sessions[index], start_s, end_s)
    result["log"] = str(path)
    result["session"] = index
    result["sessions"] = len(sessions)
    return result


def load_run(path):
    """A parse result (JSON, or a bench run.json holding one under 'metrics') or a log."""
    if str(path).endswith(".json"):
        with open(path, encoding="utf-8") as stream:
            data = json.load(stream)
        return data.get("metrics", data)
    return parse_log(path)


def flatten(value, prefix=""):
    """Numeric leaves as dotted paths, in a stable order."""
    if isinstance(value, bool) or value is None:
        return {}
    if isinstance(value, (int, float)):
        return {prefix: float(value)}
    if isinstance(value, dict):
        out = {}
        for key in value:
            if key in ("sources", "window_s", "memory", "session", "sessions"):
                continue
            out.update(flatten(value[key], f"{prefix}.{key}" if prefix else key))
        return out
    return {}


def summarize(runs):
    columns = {}
    for run in runs:
        for key, value in flatten(run).items():
            columns.setdefault(key, []).append(value)
    return {key: {"mean": statistics.fmean(values), "min": min(values), "max": max(values),
                  "n": len(values)} for key, values in columns.items()}


def compare(a_runs, b_runs, label_a="A", label_b="B"):
    a = summarize(a_runs)
    b = summarize(b_runs)
    keys = list(a) + [key for key in b if key not in a]

    def cell(stats):
        if stats is None:
            return "n/a"
        if stats["n"] == 1:
            return f"{stats['mean']:.4g}"
        return f"{stats['mean']:.4g} ({stats['min']:.4g}-{stats['max']:.4g})"

    rows = [f"| metric | {label_a} (n={len(a_runs)}) | {label_b} (n={len(b_runs)}) | delta | delta % |",
            "|---|---|---|---|---|"]
    for key in keys:
        left = a.get(key)
        right = b.get(key)
        delta = percent = ""
        if left and right:
            difference = right["mean"] - left["mean"]
            delta = f"{difference:+.4g}"
            percent = f"{difference / left['mean'] * 100:+.1f}%" if left["mean"] else ""
        rows.append(f"| {key} | {cell(left)} | {cell(right)} | {delta} | {percent} |")
    return "\n".join(rows) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    parse = commands.add_parser("parse", help="metrics of one log as JSON")
    parse.add_argument("log")
    parse.add_argument("--session", type=int, help="session index (default: the last)")
    parse.add_argument("--start-s", type=float, help="window start, log seconds")
    parse.add_argument("--end-s", type=float, help="window end, log seconds")
    diff = commands.add_parser("compare", help="markdown table of A against B")
    diff.add_argument("--a", nargs="+", required=True)
    diff.add_argument("--b", nargs="+", required=True)
    diff.add_argument("--label-a", default="A")
    diff.add_argument("--label-b", default="B")
    args = parser.parse_args(argv)
    if args.command == "parse":
        result = parse_log(args.log, args.session, args.start_s, args.end_s)
        json.dump(result, sys.stdout, indent=1)
        sys.stdout.write("\n")
        return 0
    sys.stdout.write(compare([load_run(p) for p in args.a], [load_run(p) for p in args.b],
                             args.label_a, args.label_b))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
