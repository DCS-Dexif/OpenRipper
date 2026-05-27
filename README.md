# OpenRipper

A free, open-source toolkit for extracting 3D assets — meshes, textures,
materials, and shader inputs — from running graphics applications. Built as a
clean, modular, GPL-licensed alternative to closed and paywalled ripping
tools, intended for **personal archival, game preservation, modding, and
education**.

> OpenRipper is **not** affiliated with Ninja Ripper or any other extraction
> tool. Use it only on software you own or have explicit permission to
> inspect. Use for piracy, IP theft, or any unlawful purpose is strictly
> prohibited and outside the scope of this project.

## Status

Pre-alpha, Stage 5. Stages 1–4 are complete: build system, DLL injection,
D3D11 full capture pipeline (vertex/index, textures, materials, hotkey, overlay,
session output). Stage 5 ships the **D3D12** and **D3D9** backends.

See [docs/ROADMAP.md](docs/ROADMAP.md) for the full staged plan.

## Backends

| API          | Status    |
| ------------ | --------- |
| Direct3D 11  | complete  |
| Direct3D 12  | complete  |
| Direct3D 9   | complete  |
| OpenGL 3.3+  | planned   |
| Vulkan 1.x   | planned   |

## Quick build (Windows / MSVC)

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build pulls [MinHook](https://github.com/TsudaKageyu/minhook) automatically
via CMake `FetchContent`. No vcpkg/conan setup is required for the default
configuration.

Artifacts land under `build/bin/Release/`:

* `OpenRipper.exe`       — CLI launcher / injector
* `OpenRipper_d3d11.dll` — D3D11 capture backend
* `OpenRipper_d3d12.dll` — D3D12 capture backend
* `OpenRipper_d3d9.dll`  — D3D9 capture backend

Full instructions: [docs/BUILD.md](docs/BUILD.md).

## Quick capture

```bat
build\bin\Release\OpenRipper.exe --target "C:\Path\To\YourGame.exe" --backend d3d11
```

Launch the game through OpenRipper (or attach by PID with `--pid`), then press
**F10** to trigger a rip. Assets appear in `captures/<timestamp>/` next to the
game EXE:

```
captures/20260521_140211/
  frame000120_draw00000.obj
  frame000120_draw00000_ps_t0.dds
  frame000120_materials.json
  session.json
```

The overlay flashes **"CAPTURED — frame NNNNNN"** on-screen for ~2 seconds
after each successful rip. See [docs/BUILD.md](docs/BUILD.md) for full config
reference (`OpenRipper.cfg`).

## Documentation

* [docs/BUILD.md](docs/BUILD.md) — build requirements, CMake options, and config reference.
* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — module layout and hooking strategy.
* [docs/ROADMAP.md](docs/ROADMAP.md) — staged feature plan.

## License

GNU GPL v3 — see [LICENSE](LICENSE). The copyleft is deliberate: any
derivative work must remain free, so the tool cannot be re-paywalled.

## Contributing

The project is in active development through Stage 5. Issues and PRs are
welcome. Please keep contributions focused on archival / preservation /
interoperability use cases and free-software principles.
