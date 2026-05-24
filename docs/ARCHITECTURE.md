# OpenRipper Architecture

OpenRipper is split into three layers: an **out-of-process launcher**, a
shared **core** of utilities, and a swappable **backend DLL** per graphics
API. The launcher never touches the GPU; every hook lives inside a backend
DLL loaded into the target process.

```
+--------------------------+        +----------------------------+
|  OpenRipper.exe (CLI)    |        | OpenRipper_d3d11.dll       |
|  - parse args            |        | OpenRipper_d3d12.dll       |
|  - launch target         |        | OpenRipper_vulkan.dll      |
|  - inject backend DLL    |        |     ...                    |
+------------+-------------+        | (loaded into game process) |
             |                      +----------------------------+
             v                                    ^
   target.exe (suspended)  --resume-->  game runs with hooks live
```

## Directory layout

| Path                  | Contents |
| --------------------- | -------- |
| `cmake/`              | Reusable CMake helpers (flags, dependency fetch). |
| `docs/`               | Human-facing documentation. |
| `src/core/`           | Logger, config loader, common capture types. Header-friendly; static-linked into every backend and the CLI. |
| `src/runtime/`        | Out-of-process helpers: process launching, DLL injection. |
| `src/cli/`            | `OpenRipper.exe` — command-line front-end. |
| `src/backends/<api>/` | One DLL per graphics API. Each backend installs hooks via MinHook and writes captured assets through the exporters. |
| `src/exporters/`      | Format writers: OBJ today; glTF, DDS, PNG, JSON sidecar later. |
| `src/gui/`            | (planned) Dear ImGui front-end. |
| `tools/`              | (planned) standalone converters and Blender/Noesis importers. |

## Hooking strategy

* **Default — in-process inline hooks via MinHook.** The backend DLL is loaded
  into the target with classic `CreateRemoteThread` + `LoadLibraryW`
  injection. Hook addresses are obtained from the vtables of throw-away
  device / swap-chain objects created at backend init (see
  `src/backends/d3d11/hooks_d3d11.cpp`).
* **Future — optional proxy DLL deployments.** Drop-in `dxgi.dll` /
  `d3d11.dll` shims placed next to the game executable, for processes that
  resist injection or that load graphics modules before our injection thread
  runs.
* **Future — vendor-recognized layers.** Vulkan implicit layer JSON
  manifests, D3D12 PIX-style capture replay, etc., where they are lighter
  and more robust than raw vtable hooks.

## Threading & re-entrancy

Hook callbacks run on the target's render thread(s) and must:

1. Take no long locks. Heavy work (GPU readback, file I/O, format encoding)
   is queued onto a dedicated rip thread via an SPSC ring.
2. Never call into the graphics API recursively from inside a hook prologue;
   the device's internal lock may already be held.
3. Tolerate host re-entry. Some titles wrap their own D3D pipeline, so the
   same hook may be invoked twice on the same thread. Re-entry counters
   guard the slow path.

## Capture model

Each "rip" is a snapshot tree:

```
RipSession
+- Frame N
|   +- DrawCall 0  -> MeshSnapshot + bound textures + shader hash
|   +- DrawCall 1  -> ...
|   +- ...
+- manifest.json
```

`MeshSnapshot` (in `src/core/types.hpp`) is API-agnostic: positions, normals,
UVs, tangents, blend indices/weights, plus the raw vertex layout for
round-tripping. Exporters consume `MeshSnapshot` without knowing which
backend produced it.

`TextureSnapshot` mirrors that contract for textures: width, height, mip
count, array size, native format token, and raw pixels.

## Capture pipeline (Stage 2)

When `capture_frame=N` is set in `OpenRipper.cfg`, the D3D11 backend captures
every draw call between `Present(N-1)` and `Present(N)`:

```
hooked_present (frame N-1) ─► set g_capture_active = true
    │
    ▼  (app draws frame N)
hooked_Draw* ──► capture_draw()
    │               1. ctx->GetDevice()
    │               2. IAGetPrimitiveTopology / IAGetInputLayout /
    │                  IAGetIndexBuffer / IAGetVertexBuffers
    │               3. lookup_input_layout() — decode D3D11_INPUT_ELEMENT_DESC[]
    │               4. CreateBuffer(STAGING) + CopyResource + Map/memcpy/Unmap
    │               5. return MeshSnapshot
    ▼
write_obj(MeshSnapshot)  ─►  captures/frame######_draw#####.obj
    │
    ▼ (more draws, each gets its own .obj)
hooked_present (frame N) ──► set g_capture_active = false
```

**Input-layout registry** (`src/backends/d3d11/state_d3d11.cpp`): because
`ID3D11InputLayout` is opaque, we hook `ID3D11Device::CreateInputLayout`
(vtable[11]) and store a deep copy of every `D3D11_INPUT_ELEMENT_DESC[]` keyed
by layout pointer. Draw hooks look up the layout at capture time.

**Immediate context only (Stage 2)**: `ctx->GetType()` is checked; deferred
contexts are skipped with a one-time warning. Deferred-context capture (needed
for command-list based renderers) is deferred to a future stage.

**Trigger**: `g_capture_active` is an atomic flag in `hooks_d3d11.cpp` written
by `hooked_present`. Stage 4 hotkey code will also write this flag; the flag
is the only interface between the trigger source and the capture path.

## Anti-cheat & detection

OpenRipper is **not** designed to evade anti-cheat. Hooking a process
protected by EAC, BattlEye, VAC, Vanguard, etc. will likely terminate the
game and may violate the title's terms of service. Future backend init
should detect known anti-cheat modules and refuse to attach with a clear
warning. Users wishing to extract from such titles should pursue the
publisher for an authorized extraction tool or work from installed asset
files offline.

## Why GPL?

OpenRipper exists because the dominant tool in this niche is closed and paywalled,
and game preservation suffers when extraction know-how is locked behind a wall.
Copyleft prevents anyone — including future maintainers — from re-closing the
project.
