# Package an opt-in Star Fox Aftermath diagnostic

This package collects evidence for the RTX 5090 device loss. It does not fix
that loss, change airplane mode, or include the separate command-300 service
fix from PR #64.

## Build with the SDK enabled

The ordinary Windows build does not define `HAS_NSIGHT_AFTERMATH`. Its
tracker is a no-op even when the INI setting is true. The SDK headers and an
enabled rebuild are required, not just the DLL.

The official Windows x64 SDK is
[Nsight Aftermath 2026.3.0.26197](https://developer.nvidia.com/downloads/assets/tools/secure/nsight-aftermath-sdk/2026_3_0/windows_x64/NVIDIA_Nsight_Aftermath_SDK_2026.3.0.26197-windows_x64.zip).
An unauthenticated download succeeded on 2026-11-04.

| File | SHA256 |
| --- | --- |
| Official SDK ZIP | `e38136a60110199559b7365d3ea4ec0cb5588dc2b0f593877d864e0299659a3f` |
| `lib/x64/GFSDK_Aftermath_Lib.x64.dll` | `e119d214ecbc8abbbe9f3dc4990a77b999e4fd268071e0a1145c25e487cc0dd7` |

No SDK or DLL was found in the checked `C:/lx`, NVIDIA installation, or
existing diagnostic-package locations. Use a separate build directory to
avoid leaving Aftermath enabled in the ordinary build's CMake cache. In a
builder-owned job with the source pinned to this branch, retain the flags
from `C:/lx/configure.sh`, change its `-B` directory to
`C:/lx/build-aftermath`, and add `-DENABLE_NSIGHT_AFTERMATH:BOOL=ON`.
Set this process-local environment variable before configuring:

```sh
export NSIGHT_AFTERMATH_SDK='C:/lx/deps/nsight-aftermath-2026.3.0'
sed \
  -e 's@-B C:/lx/build @-B C:/lx/build-aftermath @' \
  -e 's@-DCMAKE_BUILD_TYPE=RelWithDebInfo@-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_NSIGHT_AFTERMATH:BOOL=ON@' \
  C:/lx/configure.sh > C:/lx/configure-aftermath.sh
bash C:/lx/configure-aftermath.sh
cmake --build C:/lx/build-aftermath --parallel 12
```

That directory must contain `include/GFSDK_Aftermath_GpuCrashDump.h`. Build
with `cmake --build C:/lx/build-aftermath --parallel 12`. Verify that the
tracker's compiler command contains `HAS_NSIGHT_AFTERMATH` and the SDK
include directory. The enabled tracker was syntax-checked against these
real SDK headers on Linux; a Windows build and runtime check are still
required.

## Build in CI

Run release-binaries by hand with `nsight_aftermath` on and `publish_release`
off. The Windows builds fetch the SDK above, check its SHA256, keep only
`include/` and configure with `-DENABLE_NSIGHT_AFTERMATH=ON`. Their
artifacts are named `lichii-windows-x86_64[-pgo]-aftermath-<sha>` and hold
no NVIDIA files, so the tester puts `GFSDK_Aftermath_Lib.x64.dll` from his
own SDK download beside `lichii.exe`. Without it the log shows
`Failed to load Nsight Aftermath DLL` and no dumps are written. The run
refuses to publish a release.

## Assemble the private portable launch

Place the unmodified `GFSDK_Aftermath_Lib.x64.dll` beside `lichii.exe` in
the diagnostic payload. The DLL is loaded dynamically, so the ordinary PE
import-closure packager will not discover it automatically.
Its PE import table lists only Windows system DLLs; no additional runtime
redistributable was identified there.

The SDK's `LICENSE`, supplement section 1, explicitly lists DLL/SO files
as distributable under its terms. Include the complete license and usage
guidelines in the app package, for example as
`licenses/NVIDIA-Nsight-Aftermath-LICENSE.txt` and
`licenses/NVIDIA-Nsight-Aftermath-usage-guidelines.txt`. Do not ship the SDK
headers or relicense the proprietary DLL as GPL. The license's distribution
requirements and section 2.5 still apply; this is not a blanket license
compatibility determination.

Do not use the normal `launch-template.ps1` unchanged. It edits the shared
owner INI temporarily, restores it afterward, and copies only one top-level
log. The diagnostic launch must instead use a fresh run directory containing
the executable, its dependencies, and a real portable `user` directory.

1. Create a new run directory for each process, such as
   `runs/<timestamp>-<unique-id>`. Do not reuse an earlier run directory.
2. Copy the reporter's configuration into that run's `user/config` locally,
   including per-game and input profiles. Neither `user/config` nor
   `user/log` may be a junction to shared data. Keep personal configuration,
   keys, firmware and saves out of the distributed ZIP.
3. Reuse the reporter's required local game data without moving it. If
   using directory junctions, limit them to data directories, never the
   whole `user` directory, `config`, or `log`. Preserve the game update,
   renderer settings and airplane-mode setting.
4. In the private `user/config/qt-config.ini`, replace only these rows in
   their existing sections. Do not append duplicate sections:

```ini
[Renderer]
nsight_aftermath\default=false
nsight_aftermath=true

[Miscellaneous]
log_filter\default=false
log_filter=*:Info
```

The configuration reader requires `\default=false` or it resets the value
to its default. This setting is not switchable per game, so a per-game INI
alone cannot enable it. The equivalent override is
`Renderer/nsight_aftermath=true`, but apply it to the private copied INI,
not through the existing shared-config launcher. There is no Aftermath
environment-variable override in the current frontend.

From the fresh run directory, launch the GUI with the explicit game path:

```powershell
.\lichii.exe -g "D:\path\to\Star Fox\exefs\main"
```

On Windows, portable data is resolved beside the executable, not merely
from the working directory. Do not point at an executable in another folder
and expect a local `user` directory to isolate it. The Qt frontend does not
consume the CLI frontend's `--config` option.

## Verify capture and retain the result

The game startup log must show `Renderer.nsight_aftermath: true` and:

```text
Nsight Aftermath dump directory is ".../user/log/gpucrash/<unique-id>"
```

The marker is emitted only after the DLL loads, required exports resolve,
the directory is created, and `GFSDK_Aftermath_EnableGpuCrashDumps` succeeds.
The device must support `VK_NV_device_diagnostics_config`. The 2026.3 SDK
release notes target R610 and newer drivers; the reported 621.1 driver meets
that version requirement. Do not change a driver merely for this capture.

Keep ordinary GPU logging and Vulkan validation unchanged for the first
capture. Aftermath does not require them: shader SPIR-V is saved through
its own tracker even when `gpu_log_level=Off`. An absent startup marker is
not a successful capture setup. Check the explicit DLL/export/API errors
and, without modifying it, the user's NVIDIA Aftermath `ForceOff` setting.

Wait at least 15 seconds after device loss before closing the process.
Archive the entire private `user/log` directory, not just `eden_log.txt`,
plus the exact executable commit and DLL hash. Do not archive `user/keys`,
NAND, saves, or personal configuration for sharing.

Each tracker initialization now creates a fresh directory atomically, even
when a game is restarted in the same process. Legacy captures and other
initializations are not removed. The raw `.nv-gpudmp` is written before any
JSON decoding, so decoder creation, JSON generation, JSON retrieval, or JSON
write failures do not discard that raw evidence. Keep captures until the
investigation finishes; no automatic cleanup is introduced.

## Verification commands

`build/bin/tests '[aftermath]'` checks legacy and repeated-initialization
retention, directory errors, raw-write errors, decode failure and exceptions,
successful JSON output, and JSON-write failure. The extracted old behavior
failed both test cases before the change.
