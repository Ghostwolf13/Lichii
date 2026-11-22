# AIO-11 integration notes

This is a test branch, not a merge into `nx2`.
Base: `dc386a2ea4e3a16e1c3f81c4d90e73b7a2596ffc`.
Branch: `test/all-in-one-11`.

## Components

Each head below was merged in this order with `git merge --no-ff`.

| PR | Head | Branch |
| --- | --- | --- |
| #2121 | `23e5a91efd0dcf073bd9c0a33c8d5dee1c7e3d69` | `capy/swblitter-swizzle-with-switch` |
| #2114 | `fb5f22f191e2c5fa5629bad99031202dd953dfeb` | `capy/windows-pgo-refresh-1884` |
| #2116 | `0f979a4935f8de06fbaadffc817e0e577a38314a` | `capy/fast-draw-vertex-streams` |
| #2118 | `ae5843014a5c944e8977d8110a79eac7da052a7f` | `capy/videocore-keep-fast-draws` |
| #2143 | `5d4c88ae5c0e810388cb27ca4f75a2c1e4c8a8fb` | `capy/videocore-pass-equal-and` |
| #2131 | `87fc9409230e3ba09ad614d151d8dff0d20d17d7` | `capy/download-nextendo-bcat-packages` |
| #2148 | `27d8c3588172edbb2f3e89b70f2f64fc2af0a26e` | `capy/qlaunch-home-menu` |
| #2156 | `eed766ec376acce0d455325ea8ee233029179ed4` | `capy/zero-trim-run-lookup` |
| #2160 | `eb898f212d7f9ba91885cac1e24f681137f0bbc0` | `capy/sync-serial-once` |
| #2161 | `797a9b5fc6c75253b77d599e0d556c5d103101b2` | `capy/inline-current-tick` |
| #2164 | `ae907621af48983843e56db9f84e86c57f6ce34a` | `capy/videocore-share-pipeline-and` |
| #2162 | `f2d9fd8b893e79baab5207554dff8a88b05769a6` | `fix/task-mesh-inline-search` |
| #2166 | `42eb98f3706bdd700351f71ff1822092efc82563` | `capy/oot-default-60-vsync` |

#2166 was added after both production CI jobs and review completed successfully.

## Conflict resolutions

None. All thirteen heads merged cleanly. Automatic merges composed the changes in
`src/tests/CMakeLists.txt`, `src/tests/video_core/fast_draw.cpp`,
`src/tests/video_core/maxwell_fast_method.cpp`, `src/video_core/engines/maxwell_3d.cpp`,
`src/video_core/engines/maxwell_3d.h`, `src/video_core/nx2_fast_draw.h`,
`src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` and `src/lichii/main_window.cpp`.
The combined frontend keeps both the pre-boot BCAT preparation and HOME Menu session hand-off.
The combined fast-draw classifier keeps both dynamic vertex-stream and query-state changes.

## Exclusions

Do not merge #1810 alongside #2143: they change the same semaphore drains.
#2029 ReBAR, #2038, the lazy-untrack branch and #2108 direct NVN are excluded.
#2167 was conditional and is not included. Its CI was green, but its inline review still
had an unresolved finding about a second eviction pass after critical pressure had eased.

## Recheck ancestry and merge order

From the repository root, this prints each component's merge commit and exact second parent:

```sh
git log --reverse --first-parent --merges --format='%H %P %s' \
    dc386a2ea4e3a16e1c3f81c4d90e73b7a2596ffc..test/all-in-one-11
git diff --check dc386a2ea4e3a16e1c3f81c4d90e73b7a2596ffc..test/all-in-one-11
```

Tester guidance and final build evidence are recorded in the crew report `EGLA-316`.
