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

Artifacts land under `build/bin/`:

* `OpenRipper.exe`       — out-of-process launcher / injector.
* `OpenRipper_d3d11.dll` — D3D11 capture backend (loaded into the target).

## Build options

All options are CMake cache variables. Override with `-DNAME=ON/OFF` at
configure time:

| Option                       | Default | Purpose |
| ---------------------------- | ------- | ------- |
| `OPENRIPPER_BUILD_D3D11`     | ON      | D3D11 capture backend. |
| `OPENRIPPER_BUILD_D3D12`     | OFF     | D3D12 backend (not yet implemented). |
| `OPENRIPPER_BUILD_D3D9`      | OFF     | D3D9 backend (not yet implemented). |
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
build\bin\OpenRipper.exe --target "C:\Path\To\YourGame.exe" --backend d3d11
```

The CLI launches the target suspended, injects `OpenRipper_d3d11.dll`, and
resumes execution. The backend writes to `OpenRipper.log` next to the host
executable. To attach to an already-running process:

```bat
build\bin\OpenRipper.exe --pid 12345 --backend d3d11
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
   build\bin\OpenRipper.exe --target "C:\Path\To\YourGame.exe" --backend d3d11
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

## Common build problems

| Symptom | Cause / Fix |
| ------- | ----------- |
| `<format>: No such file or directory` | Upgrade Visual Studio to 17.4 or newer; `std::format` is C++20 and requires recent MSVC. |
| `MinHook.h: No such file or directory` | First-time configure: run with internet access so `FetchContent` can clone MinHook. Subsequent builds work offline. |
| Linker error `d3d11.lib not found` | Install the Windows 10 SDK (Visual Studio Installer → Individual Components). |
| `MH_Initialize` returns non-zero at runtime | Another hooking library (RenderDoc, ReShade, EAC, etc.) is already attached to the target. Disable the conflicting tool. |
