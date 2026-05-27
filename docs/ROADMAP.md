# OpenRipper Roadmap

Stages are roughly ordered by dependency. A stage is "done" when its goals
are runnable end-to-end against at least one real-world target.

## Stage 1 — Foundation

* [x] CMake build system, dependency bootstrap (MinHook via FetchContent).
* [x] Logger, config loader, common capture types (`MeshSnapshot`,
      `TextureSnapshot`).
* [x] CLI launcher with `CreateProcess` + `CreateRemoteThread` injection.
* [x] D3D11 backend DLL: vtable scan, MinHook on `Present`, `Draw`,
      `DrawIndexed`, `DrawInstanced`, `DrawIndexedInstanced`. Logs draw
      counts per frame.

## Stage 2 — Vertex/index capture (D3D11)

* [x] Snapshot `IASetVertexBuffers` / `IASetIndexBuffer` /
      `IASetInputLayout` / `IASetPrimitiveTopology` state per draw.
* [x] Copy bound vertex + index buffers from GPU to CPU (CopyResource into
      `D3D11_USAGE_STAGING`, `Map` to read).
* [x] Decode `D3D11_INPUT_ELEMENT_DESC[]` into semantic-tagged
      `MeshSnapshot::VertexAttribute` records.
* [x] OBJ exporter writes positions + normals + UVs for the captured frame.

## Stage 3 — Textures

* [x] Walk SRVs bound at draw time; dump the backing `ID3D11Texture2D` to
      DDS (preserving compressed formats: BC1-BC7) with PNG fallback for
      uncompressed surfaces. Full mipchain captured for array slice 0.
* [x] Material sidecar JSON cross-referencing textures to mesh draws.
* [ ] Optional: cubemap and array slice expansion (Stage 3.1 — deferred).

## Stage 4 — UX

* [x] Global hotkey (default F10) registered from the injected DLL to
      trigger a full-frame rip.
* [x] "Time freeze" mode (`freeze_frames=N`) that latches the next N frames
      and dumps them per trigger.
* [x] Output directory layout with per-session timestamps (`YYYYMMDD_HHMMSS/`)
      and `session.json` manifest.
* [x] On-screen indicator: D2D1 text overlay "CAPTURED — frame NNNNNN" for
      ~2 s. Two-mode rendering: direct (game device has BGRA support) or
      indirect via a helper BGRA device + CPU stamp (games that lack BGRA,
      e.g. most DX11 titles). Window-title fallback if both D2D1 paths fail.
* [x] Stage 4.1 — `time_freeze_on_rip`: when true, hooked_present skips the
      real Present call during active capture frames so the display freezes.
* [x] Stage 4.2 — `flip_winding=true` config option: reverse OBJ face winding
      order on export (for engines that use CW front-face convention).

## Stage 5 — Additional backends

* [x] D3D12 backend: vtable hooks on `IDXGISwapChain::Present`,
      `ID3D12GraphicsCommandList::DrawInstanced/DrawIndexedInstanced` and
      supporting state-tracking hooks (IASetVertexBuffers, IASetIndexBuffer,
      SetPipelineState, SetDescriptorHeaps, ExecuteCommandLists, etc.).
      Two-phase capture: UPLOAD-heap draws copied immediately; DEFAULT-heap
      resources fence-drained + readback at Present. D3D11On12 overlay.
* [x] D3D9 backend (vtable hook on `IDirect3DDevice9::DrawIndexedPrimitive`,
      `DrawPrimitive`, and UP variants; FVF + vertex-declaration decoding).
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
