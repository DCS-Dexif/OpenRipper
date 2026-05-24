// OpenRipper - src/backends/d3d11/state_d3d11.hpp
//
// Input-layout registry.
//
// ID3D11InputLayout is an opaque handle; the D3D11 API provides no way to
// query back the D3D11_INPUT_ELEMENT_DESC[] that was used to create it. This
// registry fixes that by intercepting ID3D11Device::CreateInputLayout (vtable
// slot 11) and storing a deep copy of every desc array, keyed by the returned
// layout pointer.
//
// Thread safety: all three public functions acquire an internal mutex so they
// may be called from multiple render threads concurrently.

#pragma once

#include <d3d11.h>
#include <optional>
#include <string>
#include <vector>

namespace openripper::backends::d3d11 {

// Self-contained description of one input layout's attributes. SemanticName
// strings are owned by name_storage so the struct is safe to copy.
struct InputLayoutDesc {
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string>              name_storage;
};

// Called from the hooked CreateInputLayout thunk after the real call succeeds.
// Makes a deep copy of desc[0..count-1] and stores it under layout.
void register_input_layout(ID3D11InputLayout*              layout,
                            const D3D11_INPUT_ELEMENT_DESC* desc,
                            UINT                            count);

// Remove the entry for layout (Stage 2 does not hook Release, so this is
// called manually from remove_hooks to clear stale entries on DLL unload).
void forget_all_input_layouts();

// Thread-safe lookup. Returns std::nullopt if layout was never registered
// (created before the hook was installed, or already forgotten).
std::optional<InputLayoutDesc> lookup_input_layout(ID3D11InputLayout* layout);

} // namespace openripper::backends::d3d11
