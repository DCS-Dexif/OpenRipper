// OpenRipper - src/backends/d3d11/state_d3d11.cpp

#include "state_d3d11.hpp"

#include <mutex>
#include <unordered_map>

namespace openripper::backends::d3d11 {
namespace {

std::mutex                                              g_map_mu;
std::unordered_map<ID3D11InputLayout*, InputLayoutDesc> g_map;

} // namespace

void register_input_layout(ID3D11InputLayout*              layout,
                            const D3D11_INPUT_ELEMENT_DESC* desc,
                            UINT                            count)
{
    InputLayoutDesc entry;
    entry.elements.resize(count);
    entry.name_storage.resize(count);

    for (UINT i = 0; i < count; ++i) {
        // Deep-copy the semantic name string so this entry is self-contained.
        entry.name_storage[i]              = desc[i].SemanticName;
        entry.elements[i]                  = desc[i];
        entry.elements[i].SemanticName     = entry.name_storage[i].c_str();
    }

    std::lock_guard lock(g_map_mu);
    g_map.insert_or_assign(layout, std::move(entry));
}

void forget_all_input_layouts() {
    std::lock_guard lock(g_map_mu);
    g_map.clear();
}

std::optional<InputLayoutDesc> lookup_input_layout(ID3D11InputLayout* layout) {
    std::lock_guard lock(g_map_mu);
    if (const auto it = g_map.find(layout); it != g_map.end())
        return it->second;   // copy-out (callers keep it only for the duration of one draw)
    return std::nullopt;
}

} // namespace openripper::backends::d3d11
