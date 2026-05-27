# Building OpenRipper

OpenRipper currently targets **Windows 10/11 (x64)** as its primary platform.
Linux support via Wine/Proton + DXVK is a longer-term goal (see
[ROADMAP.md](ROADMAP.md)).

## Prerequisites

| Requirement                              | Notes |
| ---------------------------------------- | ----- |
| Windows 10 SDK 10.0.19041 or newer       | Ships with Visual Studio. |
| Visual Studio 2022 (17.4+) or Build Tools | MSVC v143, C++ workload. `std::format` requires 17.4+. |
| CMake 3.21 or newer                      | https://cmake.org/download/ |
| Git                                      | Required for `FetchContent` (MinHook). |

Optional:

* **Ninja** for faster builds: `cmake -S . -B build -G Ninja`
* **clang-cl** as a drop-in MSVC replacement is supported but untested.

## Configure & build

From a *Developer Command Prompt for VS 2022* (or any shell with `cmake` and
`cl.exe` on `PATH`):

```bat
git clone https://github.com/<you>/OpenRipper.git
cd OpenRipper
cmake -S . -B build -A x64
cmake --build build --config Release -j
```

Artifacts land under `build/bin/Release/`:

* `OpenRipper.exe`       — out-of-process launcher / injector.
* `OpenRipper_d3d11.dll` — D3D11 capture backend (loaded into the target).
* `OpenRipper_d3d12.dll` — D3D12 capture backend.
* `OpenRipper_d3d9.dll`  — D3D9 capture backend.

## Build options

All options are CMake cache variables. Override with `-DNAME=ON/OFF` at
configure time:

| Option                       | Default | Purpose |
| ---------------------------- | ------- | ------- |
| `OPENRIPPER_BUILD_D3D11`     | ON      | D3D11 capture backend. |
| `OPENRIPPER_BUILD_D3D12`     | ON      | D3D12 capture backend (Stage 5). |
| `OPENRIPPER_BUILD_D3D9`      | ON      | D3D9 capture backend. |
| `OPENRIPPER_BUILD_OPENGL`    | OFF     | OpenGL backend (not yet implemented). |
| `OPENRIPPER_BUILD_VULKAN`    | OFF     | Vulkan backend (not yet implemented). |
| `OPENRIPPER_BUILD_CLI`       | ON      | Command-line launcher. |
| `OPENRIPPER_BUILD_GUI`       | OFF     | Dear ImGui front-end (not yet implemented). |
| `OPENRIPPER_BUILD_TESTS`     | OFF     | Unit tests (not yet implemented). |

Example — minimal build with only the CLI and D3D11 backend (the default):

```bat
cmake -S . -B build -A x64 -DOPENRIPPER_BUILD_GUI=OFF -DOPENRIPPER_BUILD_TESTS=OFF
```

## Running (smoke test)

```bat
build\bin\Release\OpenRipper.exe --target "C:\Path\To\YourGame.exe" --backend d3d11
```

The CLI launches the target suspended, injects the backend DLL, and resumes
execution. The backend writes to `OpenRipper.log` next to the host executable.

Select the right backend for the game's API:

```bat
# Direct3D 11 (most PC titles 2009–present)
build\bin\Release\OpenRipper.exe --target "Game.exe" --backend d3d11

# Direct3D 12 (modern DX12 titles)
build\bin\Release\OpenRipper.exe --target "Game.exe" --backend d3d12

# Direct3D 9 (older titles, many emulators)
build\bin\Release\OpenRipper.exe --target "Game.exe" --backend d3d9
```

To attach to an already-running process:

```bat
build\bin\Release\OpenRipper.exe --pid 12345 --backend d3d11
```

## Config reference (`OpenRipper.cfg`)

Place `OpenRipper.cfg` next to the target EXE. Format: `key=value`, one per
line. Lines beginning with `#` are comments. All keys are optional; unrecognised
keys are silently ignored (logged at `trace` level).

| Key | Default | Description |
| --- | ------- | ----------- |
| `output_dir` | `captures` | Root directory for session output. A timestamped subdirectory (`YYYYMMDD_HHMMSS/`) is created inside it per run. |
| `log_file` | `OpenRipper.log` | Path of the rolling log file, relative to the target EXE. Empty string disables the file sink. |
| `log_level` | `info` | Verbosity floor. Values: `trace` `debug` `info` `warn` `error` `off`. Use `debug` to see per-draw layout lines. |
| `rip_hotkey` | `0x79` | Virtual-key code (VK_*) for the in-game rip hotkey. `0x79` = F10. Accepts hex (`0x79`) or decimal (`121`). `0` disables the hotkey. |
| `capture_frame` | *(disabled)* | 0-based frame index (counting from the first `Present` after DLL load) on which to auto-trigger a capture. Omit or leave unset to use hotkey-only. |
| `freeze_frames` | `1` | Number of consecutive frames to capture per trigger. `1` = single frame; `N` = burst of N frames (output named `frame######_*` per frame). |
| `time_freeze_on_rip` | `false` | When `true`, the backend skips calling the real `Present` during active capture frames so the display holds on the last pre-capture frame. The game's render thread continues submitting draw calls (which are captured), but no new frame appears until capture finishes. |
| `flip_winding` | `false` | When `true`, reverses OBJ face winding order on export (`a,b,c` → `a,c,b`). Use for engines that expect CW front-face convention, where imported meshes appear inside-out in Blender/Maya without this flag. |
| `dedup` | `false` | When `true`, skips GPU readback and file writes for textures whose API resource pointer was already captured earlier in the same frame. The material manifest references the first-written filename for all duplicate slots. Reduces output size significantly in games that share textures across many draw calls (atlases, shadow maps). |

**Example — hotkey-only, 2-frame burst, debug logging:**

```ini
output_dir=captures
log_level=debug
freeze_frames=2
# rip_hotkey=0x79  # F10 (default)
```

**Example — auto-capture at frame 120:**

```ini
capture_frame=120
output_dir=captures
log_level=info
```

## Smoke test — Stage 2 vertex/index capture

1. **Write a config file** next to the target EXE:

   ```ini
   # OpenRipper.cfg
   capture_frame=120
   output_dir=captures
   log_level=info
   ```

   Frame 120 is ~2 seconds into a 60 Hz title — enough time for the title
   screen to settle. Adjust as needed.

2. **Launch**:

   ```bat
   build\bin\Release\OpenRipper.exe --target "C:\Path\To\YourGame.exe" --backend d3d11
   ```

3. **Expected log output** (in `OpenRipper.log` next to the EXE):

   ```
   [INFO] OpenRipper_d3d11.dll loaded into: YourGame.exe
   [INFO] config loaded from OpenRipper.cfg
   [INFO] capture output dir: C:\Path\To\captures
   [INFO] capture scheduled for frame 120
   [INFO] D3D11 hooks installed (Present + CreateInputLayout + 4 Draw variants).
   [DEBUG] frame 0 - 47 draw calls in last frame
   ...
   [INFO] capture: frame 000120 begin - capturing all draws
   [INFO] capture: wrote frame000120_draw00000.obj
   [INFO] capture: wrote frame000120_draw00001.obj
   ...
   [INFO] capture: frame 000120 complete (N draws written)
   ```

4. **Open an OBJ in Blender**: `File → Import → Wavefront (.obj)`. A mesh
   with correct positions, normals, and UV layout should appear.

5. **Regression check — no config**: delete `OpenRipper.cfg` and relaunch.
   Log should show draw-count lines only, no capture activity, no crash.

## Smoke test — Stage 3 texture capture

Uses the same `OpenRipper.cfg` as the Stage 2 test. The test target must
bind at least one PS texture SRV before the captured draw.

1. **Config** (same as Stage 2):

   ```ini
   capture_frame=120
   output_dir=captures
   log_level=info
   ```

2. **Launch**:

   ```bat
   build\bin\Release\OpenRipper.exe --target "C:\Path\To\YourGame.exe" --backend d3d11
   ```

3. **Expected additional output** (alongside the Stage 2 OBJ files):

   ```
   [INFO] png: wrote frame000120_draw00000_ps_t0.png (2x2, dxgi=87)
   [INFO] material: wrote frame000120_materials.json (N draw records)
   ```

   Or for BC-compressed textures in a real game:

   ```
   [WARN] png: unsupported format 71 in '...' - caller should use DDS
   [INFO] dds: wrote frame000120_draw00000_ps_t0.dds (1024x1024, 11 mips, dxgi=71)
   ```

4. **Verify outputs**:
   - `captures/frame000120_materials.json` — valid JSON; open in any text editor.
     `draws[0].ps_textures[0].file` must match the PNG/DDS filename on disk.
   - `captures/frame000120_draw00000_ps_t0.png` — open in Windows Photos or Paint.
     Should show the correct texture colour.
   - For DDS files: open in Paint.NET (free DDS plugin) or run
     `texconv -nologo frame000120_draw00000_ps_t0.dds` to inspect header.

5. **Regression check — no textures bound**: run against a draw with no PS
   SRVs. Log shows `ps_textures: []` (or 0 draw records with textures) in the
   manifest; no `_ps_t*` files written; no crash.

## Smoke test — Stage 4 UX

Uses the same `d3d11_cube` test target (1500-frame run ≈ 25 s — enough time to
press F10 manually).

### Automated: config trigger + session dir

Config (`OpenRipper.cfg`):

```ini
capture_frame=120
output_dir=captures
log_level=debug
```

Expected outputs under `captures/YYYYMMDD_HHMMSS/`:

```
frame000120_draw00000.obj
frame000120_draw00000_ps_t0.png
frame000120_materials.json
session.json
```

`session.json` must be valid JSON; `frames_captured[0].frame` must equal 120.

### Manual: F10 hotkey

Remove `capture_frame` from the config (or comment it out). Launch the test
with OpenRipper, wait for the window to appear, press **F10**. Within ~100 ms
a new `frame######_*` set appears in the session directory and the overlay text
`CAPTURED — frame NNNNNN` is visible in the window for ~2 seconds.

### Multi-frame freeze

```ini
capture_frame=120
freeze_frames=3
output_dir=captures
log_level=debug
```

Expected: `frame000120_*`, `frame000121_*`, and `frame000122_*` sets under the
session directory, plus `session.json` listing all three frames.

### Regression: Stage 2 / Stage 3 OBJ + PNG

The session-dir layout changes the capture output path. Verify that existing
Stage 2/3 files (`*.obj`, `*.png`, `*_materials.json`) are still written
correctly inside the new session subdirectory.

## Common build problems

| Symptom | Cause / Fix |
| ------- | ----------- |
| `d2d1.lib not found` | Install the Windows 10 SDK (Visual Studio Installer → Individual Components → Windows 10 SDK). d2d1.lib ships with the SDK. |
| `<format>: No such file or directory` | Upgrade Visual Studio to 17.4 or newer; `std::format` is C++20 and requires recent MSVC. |
| `MinHook.h: No such file or directory` | First-time configure: run with internet access so `FetchContent` can clone MinHook. Subsequent builds work offline. |
| Linker error `d3d11.lib not found` | Install the Windows 10 SDK (Visual Studio Installer → Individual Components). |
| `MH_Initialize` returns non-zero at runtime | Another hooking library (RenderDoc, ReShade, EAC, etc.) is already attached to the target. Disable the conflicting tool. |
