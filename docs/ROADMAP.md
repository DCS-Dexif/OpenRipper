# OpenRipper Roadmap

Stages are roughly ordered by dependency. A stage is "done" when its goals
are runnable end-to-end against at least one real-world target.

## Stage 1 — Foundation (current)

* [x] CMake build system, dependency bootstrap (MinHook via FetchContent).
* [x] Logger, config loader, common capture types (`MeshSnapshot`,
      `TextureSnapshot`).
* [x] CLI launcher with `CreateProcess` + `CreateRemoteThread` injection.
* [x] D3D11 backend DLL: vtable scan, MinHook on `Present`, `Draw`,
      `DrawIndexed`, `DrawInstanced`, `DrawIndexedInstanced`. Logs draw
      counts per frame.

## Stage 2 — Vertex/index capture (D3D11)

* [ ] Snapshot `IASetVertexBuffers` / `IASetIndexBuffer` /
      `IASetInputLayout` / `IASetPrimitiveTopology` state per draw.
* [ ] Copy bound vertex + index buffers from GPU to CPU (CopyResource into
      `D3D11_USAGE_STAGING`, `Map` to read).
* [ ] Decode `D3D11_INPUT_ELEMENT_DESC[]` into semantic-tagged
      `MeshSnapshot::VertexAttribute` records.
* [ ] OBJ exporter writes positions + normals + UVs for the captured frame.

## Stage 3 — Textures

* [ ] Walk SRVs bound at draw time; dump the backing `ID3D11Texture2D` to
      DDS (preserving compressed formats: BC1-BC7) with PNG fallback for
      uncompressed surfaces.
* [ ] Material sidecar JSON cross-referencing textures to mesh draws.
* [ ] Optional: cubemap and array slice expansion.

## Stage 4 — UX

* [ ] Global hotkey (default F10) registered from the injected DLL to
      trigger a full-frame rip.
* [ ] "Time freeze" mode that latches the next N frames and dumps them.
* [ ] Output directory layout with per-session timestamps and manifest.
* [ ] On-screen indicator (text overlay) confirming capture occurred.

## Stage 5 — Additional backends

* [ ] D3D12 backend (PIX-style command-list capture; resource state tracking).
* [ ] D3D9 backend (vtable hook on `IDirect3DDevice9::DrawIndexedPrimitive`
      and `DrawPrimitive`).
* [ ] OpenGL backend (proxy `opengl32.dll` or WGL hook).
* [ ] Vulkan backend (implicit layer manifest).

## Stage 6 — Front-end & ecosystem

* [ ] Dear ImGui in-game overlay for live capture control.
* [ ] Standalone GUI launcher (Win32 or ImGui-on-GLFW).
* [ ] Blender add-on / Noesis plugin to import OpenRipper sessions natively.
* [ ] glTF 2.0 exporter with material graph.

## Stage 7 — Advanced

* [ ] Animation / skeleton capture (constant buffer scraping + skinning
      hints; pose extraction).
* [ ] Shader-bytecode dumping + optional HLSL decompilation (via existing
      open tooling).
* [ ] World-space reconstruction from MVP matrices (auto-detect view/proj).
* [ ] Linux / Proton path via Wine winelib build of the backends.
* [ ] Headless capture from replay traces (RenderDoc capture import).
