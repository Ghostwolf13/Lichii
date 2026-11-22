#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cache_metrics  # noqa: E402

PREFIX = "HW.GPU <Info> video_core/nx2_stutter_trace.cpp:200:OnPresent: "


def line(t, message, prefix=PREFIX):
    return f"[{t:9.6f}] {prefix}{message}\n"


def stutter(t, frame, ms, cause, **categories):
    values = {name: 0.0 for name in ("gfx_miss", "compute_miss", "pipeline_wait", "shader_capture",
                                     "texture", "buffer", "query_fence", "present")}
    values.update(categories)
    fields = " ".join(f"{key}={value:.1f}" for key, value in values.items())
    return line(t, f"Nx2Stutter frame={frame} ms={ms:.1f} cause={cause} {fields} other=1.0 "
                   f"busy=front:10.0,back:{ms:.1f} builds=gfx:1,compute:0")


def summary(t, hist, builds=(2, 1), lookups=(90, 5, 3, 2, 10, 1), gfx_ms=40.0, compute_ms=5.0,
            cache_bytes=123456, driver_builds=None):
    names = ("gfx_prebuilt", "gfx_background", "gfx_recorded", "gfx_new", "compute_prebuilt",
             "compute_new")
    lookup_text = ",".join(f"{name}:{count}" for name, count in zip(names, lookups))
    hist_text = ",".join(f"{bin_ms}:{count}" for bin_ms, count in hist.items())
    return line(t, f"Nx2StutterSummary window_s=10.00 frames={sum(hist.values())} over33=0 "
                   f"over100=0 hist={hist_text} cat_ms=gfx_miss:{gfx_ms},compute_miss:{compute_ms},"
                   f"pipeline_wait:3.0,shader_capture:1.0,texture:2.0,buffer:1.0,query_fence:0.0,"
                   f"present:0.5 builds=gfx:{builds[0]},compute:{builds[1]} lookups={lookup_text} "
                   + (f"driver_builds=binary:{driver_builds[0]},compiled:{driver_builds[1]},"
                      f"rejected:{driver_builds[2]} " if driver_builds else "")
                   + f"cache_bytes={cache_bytes}")


TRACE_LOG = "".join([
    line(0.01, "Lichii Version: Lichii 1.0 | nx2-abcdef1", "Frontend <Info> qt_common.cpp:245:x: "),
    line(1.00, "Booting game: 0400486024DEC000 | Ocarina | 1.0", "Frontend <Info> main_window.cpp:1:x: "),
    line(1.10, "Nx2Boot event=boot_start mono_s=100.000 commit_mib=500.0 peak_commit_mib=510.0"),
    line(3.00, "Nx2Boot event=shader_load_begin mono_s=101.900 commit_mib=900.0 peak_commit_mib=900.0"),
    line(3.10, "Total Pipeline Count: 1870", "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
    line(9.00, "Nx2Boot event=shader_load_end mono_s=107.900 commit_mib=2000.0 peak_commit_mib=2100.0 "
               "seconds=6.000 graphics_records=1870 compute_records=300 queued=2000 skipped=170 "
               "deferred=64 cache_bytes=99000000 driver_cache_bytes=55000000"),
    line(9.00, "Shader cache v2 boot load: 6.0 s, 300 of 300 compute and 1700 of 1700 graphics "
               "pipelines built (the used list of 1700 pipelines from 2 sessions), 0 only older "
               "sessions used and 170 more cached graphics pipelines skipped at boot, 64 admitted "
               "for background prebuild (older sessions' first, then newest first), 106 left for "
               "their first draw (prebuild limit 64); driver build times: 1990 under 10 ms, 8 "
               "10-100 ms, 2 over 100 ms (4.1 s in all, longest 812.5 ms)",
         "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
    line(9.10, "Nx2Boot event=precache_begin mono_s=108.000 commit_mib=2000.0 peak_commit_mib=2100.0 jobs=5000"),
    line(199.10, "Nx2Boot event=precache_end mono_s=298.000 commit_mib=2500.0 peak_commit_mib=4107.0 "
                 "built=4990 seconds=190.000"),
    line(205.00, "Nx2Boot event=first_frame mono_s=303.900 commit_mib=2600.0 peak_commit_mib=4107.0"),
    stutter(207.0, 120, 120.0, "gfx_miss", gfx_miss=95.0),
    stutter(209.0, 230, 40.0, "texture", texture=20.0),
    line(215.00, "Runtime pipelines in the last 10 s: 3 built, 101.0 ms on the GPU thread "
                 "(0.0 ms of it graphics SPIR-V), 0.0 ms graphics SPIR-V on workers, 0.0 ms of draws "
                 "held for unbuilt pipelines", "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
    summary(215.0, {16: 560, 17: 34, 40: 1, 120: 1}),
    stutter(220.0, 900, 35.0, "pipeline_wait", pipeline_wait=12.0),
    stutter(222.0, 1000, 50.0, "other"),
    summary(225.0, {16: 590, 17: 8, 35: 1, 50: 1}, builds=(1, 0), lookups=(10, 0, 0, 1, 0, 0),
            gfx_ms=10.0, compute_ms=0.0, cache_bytes=99100000, driver_builds=(4, 2, 1)),
])

OLD_LOG = "".join([
    line(0.01, "Lichii Version: Lichii 0.9 | nx2-0000001", "Frontend <Info> qt_common.cpp:245:x: "),
    line(1.00, "Booting game: 0400C3F00006E000 | Mario Kart World | 1.0", "Frontend <Info> m.cpp:1:x: "),
    line(2.00, "Loaded Vulkan driver pipeline cache: C:/x/vulkan_pipelines.fork-v54.bin",
         "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
    line(2.50, "Total Pipeline Count: 2540", "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
    line(14.50, "Vulkan driver pipelines cached at: C:/x/vulkan_pipelines.fork-v54.bin",
         "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
    line(15.00, "Shader precache: finished, 100 built, 0 failed, 0 not reached, 42.5 s",
         "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
    line(60.00, "Graphics pipeline built on the GPU thread in 12.0 ms: pipeline=0123",
         "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
    line(61.00, "Compute pipeline built on the GPU thread in 3.0 ms: pipeline=0456",
         "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
    line(65.00, "Runtime pipelines in the last 10 s: 6 built, 30.0 ms on the GPU thread "
                "(0.0 ms of it graphics SPIR-V), 0.0 ms graphics SPIR-V on workers, 0.0 ms of draws "
                "held for unbuilt pipelines", "Render.Vulkan <Info> vk_pipeline_cache.cpp:1:x: "),
] + [line(70.0 + i, f"DKB renderer renderer_frame={i} host_pacing_mode=0 vsync_mode=0 "
                     f"cpu_frame_wall_ms={16.0 + i:.3f} cpu_composite_wall_ms=1.000",
          "Render.Vulkan <Info> renderer_vulkan.cpp:1:x: ") for i in range(5)])


class CacheMetricsTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def write(self, name, text):
        path = os.path.join(self.directory.name, name)
        with open(path, "w", encoding="utf-8") as stream:
            stream.write(text)
        return path

    def test_trace_markers(self):
        result = cache_metrics.parse_log(self.write("trace.txt", TRACE_LOG))
        self.assertTrue(result["trace"])
        self.assertEqual(result["program_id"], "0400486024DEC000")
        self.assertEqual(result["build"], "Lichii 1.0 | nx2-abcdef1")
        self.assertAlmostEqual(result["boot_to_first_frame_s"], 203.9)
        self.assertEqual(result["loading_shaders_s"], 6.0)
        self.assertEqual(result["precache_s"], 190.0)
        self.assertEqual(result["precache_peak_commit_mib"], 4107.0)
        self.assertEqual(result["records_loaded"], {"graphics": 1870, "compute": 300})
        self.assertEqual(result["cache"]["file_bytes_at_boot"], 99000000)
        self.assertEqual(result["cache"]["file_bytes_at_end"], 99100000)
        self.assertEqual(result["runtime_builds"], {"graphics": 3, "compute": 1, "total": 4})
        self.assertEqual(result["runtime_build_gpu_thread_ms"], 55.0)

    def test_frames_and_stutters(self):
        result = cache_metrics.parse_log(self.write("trace.txt", TRACE_LOG))
        self.assertEqual(result["frame_ms"]["frames"], 1196)
        self.assertEqual(result["frame_ms"]["p50"], 16.5)
        self.assertEqual(result["frame_ms"]["p99"], 17.5)
        self.assertEqual(result["frames_over_33ms"], 4)
        self.assertEqual(result["frames_over_100ms"], 1)
        stutter = result["stutter"]
        self.assertEqual(stutter["frames"], 4)
        self.assertEqual(stutter["by_group"]["translation"]["frames"], 1)
        self.assertEqual(stutter["by_group"]["translation"]["ms"], 120.0)
        self.assertEqual(stutter["by_group"]["texture_cache"]["frames"], 1)
        self.assertEqual(stutter["by_group"]["pipeline_build"]["frames"], 1)
        self.assertEqual(stutter["by_group"]["other"]["frames"], 1)
        self.assertEqual(stutter["by_cause"]["gfx_miss"]["excess_ms"], round(120.0 - 1000 / 60, 1))

    def test_percentile_in_the_overflow_bin_is_unknown(self):
        histogram = {16: 97, 40: 1, cache_metrics.OVERFLOW_BIN_MS: 2}
        self.assertEqual(cache_metrics.percentile_from_histogram(histogram, 0.50), 16.5)
        self.assertEqual(cache_metrics.percentile_from_histogram(histogram, 0.98), 40.5)
        self.assertIsNone(cache_metrics.percentile_from_histogram(histogram, 0.99))

    def test_hit_rate(self):
        result = cache_metrics.parse_log(self.write("trace.txt", TRACE_LOG))
        cache = result["cache"]
        # Graphics: 90 + 5 + 10 hits, 3 + 2 + 1 misses; compute: 10 hits, 1 miss.
        self.assertEqual(cache["graphics_hit_rate"], round(105 / 111, 4))
        self.assertEqual(cache["compute_hit_rate"], round(10 / 11, 4))
        self.assertEqual(cache["hit_rate"], round(115 / 122, 4))

    def test_driver_builds(self):
        result = cache_metrics.parse_log(self.write("trace.txt", TRACE_LOG))
        self.assertEqual(result["cache"]["driver_builds"], {"binary": 4, "compiled": 2, "rejected": 1})

    def test_driver_build_times(self):
        result = cache_metrics.parse_log(self.write("trace.txt", TRACE_LOG))
        self.assertEqual(result["driver_build_times"]["boot"],
                         {"under_10ms": 1990, "10_100ms": 8, "over_100ms": 2, "total_s": 4.1,
                          "longest_ms": 812.5})
        self.assertIsNone(result["driver_build_times"]["prebuild"])

    def test_window_limits_frames_and_stutters(self):
        result = cache_metrics.parse_log(self.write("trace.txt", TRACE_LOG), start_s=216.0)
        self.assertEqual(result["frames_over_33ms"], 2)
        self.assertEqual(result["frame_ms"]["frames"], 600)
        self.assertEqual(result["runtime_builds"]["total"], 1)

    def test_last_session_is_default(self):
        text = OLD_LOG + TRACE_LOG
        path = self.write("two.txt", text)
        self.assertEqual(cache_metrics.parse_log(path)["program_id"], "0400486024DEC000")
        first = cache_metrics.parse_log(path, session=0)
        self.assertEqual(first["program_id"], "0400C3F00006E000")
        self.assertEqual(first["sessions"], 2)

    def test_fallback_lines(self):
        result = cache_metrics.parse_log(self.write("old.txt", OLD_LOG))
        self.assertFalse(result["trace"])
        self.assertEqual(result["loading_shaders_s"], 12.5)
        self.assertIn("fallback", result["sources"]["loading_shaders_s"])
        self.assertEqual(result["precache_s"], 42.5)
        self.assertEqual(result["pipelines_queued_at_boot"], 2540)
        self.assertEqual(result["runtime_builds"], {"graphics": 1, "compute": 1, "total": 6})
        self.assertEqual(result["runtime_build_gpu_thread_ms"], 30.0)
        self.assertEqual(result["frame_ms"]["frames"], 5)
        self.assertEqual(result["frame_ms"]["p50"], 18.0)
        self.assertIsNone(result["frames_over_33ms"])
        self.assertIsNone(result["boot_to_first_frame_s"])
        self.assertIsNone(result["cache"]["hit_rate"])

    def test_ansi_and_nul_are_ignored(self):
        text = TRACE_LOG.replace("Nx2Boot event=first_frame", "\x1b[0mNx2Boot event=first_frame")
        result = cache_metrics.parse_log(self.write("ansi.txt", "\x00" + text))
        self.assertAlmostEqual(result["boot_to_first_frame_s"], 203.9)

    def test_cut_off_last_line_is_skipped(self):
        whole = cache_metrics.parse_log(self.write("whole.txt", TRACE_LOG))
        cut = (TRACE_LOG + line(225.0, "Nx2Stutter frame=99999 ms=")
               + line(225.0, "Nx2StutterSummary window_s="))
        result = cache_metrics.parse_log(self.write("cut.txt", cut))
        self.assertEqual(result["frames_over_33ms"], whole["frames_over_33ms"])
        self.assertEqual(result["frame_ms"], whole["frame_ms"])

    def test_compare_markdown(self):
        a = [cache_metrics.parse_log(self.write("a.txt", TRACE_LOG))]
        b_run = json.loads(json.dumps(a[0]))
        b_run["loading_shaders_s"] = 3.0
        b_path = self.write("b.json", json.dumps({"metrics": b_run}))
        table = cache_metrics.compare(a + a, [cache_metrics.load_run(b_path)], "old", "new")
        self.assertIn("| metric | old (n=2) | new (n=1) | delta | delta % |", table)
        self.assertIn("| loading_shaders_s | 6 (6-6) | 3 | -3 | -50.0% |", table)
        self.assertIn("stutter.by_group.translation.frames", table)

    def test_cli(self):
        path = self.write("trace.txt", TRACE_LOG)
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(cache_metrics.main(["parse", path]), 0)
        self.assertEqual(json.loads(output.getvalue())["precache_s"], 190.0)
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(cache_metrics.main(["compare", "--a", path, "--b", path]), 0)
        self.assertIn("| precache_s | 190 | 190 | +0 | +0.0% |", output.getvalue())


if __name__ == "__main__":
    unittest.main()
