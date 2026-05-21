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

Pre-alpha. Stage 1 (foundation) lands the build system, CLI launcher,
DLL-injection runtime, and a D3D11 backend that hooks `Present` / `Draw*`
and logs per-frame draw counts. Actual vertex / texture capture comes in
later stages — see [docs/ROADMAP.md](docs/ROADMAP.md).

## Backends

| API          | Status         |
| ------------ | -------------- |
| Direct3D 11  | scaffolding    |
| Direct3D 12  | planned        |
| Direct3D 9   | planned        |
| OpenGL 3.3+  | planned        |
| Vulkan 1.x   | planned        |

## Quick build (Windows / MSVC)

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build pulls [MinHook](https://github.com/TsudaKageyu/minhook) automatically
via CMake `FetchContent`. No vcpkg/conan setup is required for the default
configuration.

Artifacts land under `build/bin/`:

* `OpenRipper.exe`       — CLI launcher / injector
* `OpenRipper_d3d11.dll` — D3D11 capture backend (loaded into target processes)

Full instructions: [docs/BUILD.md](docs/BUILD.md).

## Smoke test

```bat
build\bin\OpenRipper.exe --target "C:\Path\To\YourGame.exe" --backend d3d11
```

If injection succeeds you'll see `OpenRipper.log` appear next to the target
executable, with lines such as:

```
2026-05-21 14:02:11.043 [INFO ] OpenRipper_d3d11.dll loaded into host: YourGame.exe
2026-05-21 14:02:11.180 [INFO ] D3D11 hooks installed (Present + 4 Draw variants).
2026-05-21 14:02:11.512 [DEBUG] frame 60 - 1834 draw calls (avg over recent frame)
```

## Documentation

* [docs/BUILD.md](docs/BUILD.md) — build requirements and CMake options.
* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — module layout and hooking strategy.
* [docs/ROADMAP.md](docs/ROADMAP.md) — staged feature plan.

## License

GNU GPL v3 — see [LICENSE](LICENSE). The copyleft is deliberate: any
derivative work must remain free, so the tool cannot be re-paywalled.

## Contributing

The project is in its earliest scaffolding stage; expect rough edges. Issues
and PRs are welcome once the Stage 1 base is in. Please keep contributions
focused on archival / preservation / interoperability use cases.
