// OpenRipper - src/backends/d3d12/state_d3d12.cpp

#include "state_d3d12.hpp"

#include <map>
#include <mutex>
#include <shared_mutex>

namespace openripper::backends::d3d12 {

// ---- GPU VA map -----------------------------------------------------------
namespace {
std::shared_mutex                     g_va_mu;
std::map<UINT64, VaMapEntry>          g_va_map;   // sorted by VA start

std::shared_mutex                     g_pso_mu;
std::unordered_map<ID3D12PipelineState*, PsoLayoutDesc> g_pso_map;

std::shared_mutex                     g_srv_mu;
std::unordered_map<SIZE_T, SrvEntry>  g_srv_map;

std::shared_mutex                     g_cl_mu;
std::unordered_map<ID3D12GraphicsCommandList*, PerClState> g_cl_map;

std::mutex                            g_draws_mu;
std::vector<DrawRecord>               g_draws;
} // namespace

// ---- VA map ---------------------------------------------------------------

void va_map_insert(UINT64 gpu_va, ID3D12Resource* res, UINT64 size, D3D12_HEAP_TYPE type) {
    std::unique_lock lk(g_va_mu);
    g_va_map.insert_or_assign(gpu_va, VaMapEntry{res, size, type});
}

void va_map_remove(UINT64 gpu_va) {
    std::unique_lock lk(g_va_mu);
    g_va_map.erase(gpu_va);
}

std::optional<VaMapEntry> va_map_lookup(UINT64 gpu_va) {
    std::shared_lock lk(g_va_mu);
    auto it = g_va_map.upper_bound(gpu_va);
    if (it == g_va_map.begin()) return std::nullopt;
    --it;
    if (gpu_va < it->first + it->second.size)
        return it->second;
    return std::nullopt;
}

void va_map_clear() {
    std::unique_lock lk(g_va_mu);
    g_va_map.clear();
}

// ---- PSO layout registry --------------------------------------------------

void pso_register(ID3D12PipelineState* pso,
                  const D3D12_INPUT_ELEMENT_DESC* elems, UINT count)
{
    PsoLayoutDesc entry;
    entry.elements.resize(count);
    entry.name_storage.resize(count);
    for (UINT i = 0; i < count; ++i) {
        entry.name_storage[i]          = elems[i].SemanticName;
        entry.elements[i]              = elems[i];
        entry.elements[i].SemanticName = entry.name_storage[i].c_str();
    }
    std::unique_lock lk(g_pso_mu);
    g_pso_map.insert_or_assign(pso, std::move(entry));
}

void pso_forget(ID3D12PipelineState* pso) {
    std::unique_lock lk(g_pso_mu);
    g_pso_map.erase(pso);
}

void pso_forget_all() {
    std::unique_lock lk(g_pso_mu);
    g_pso_map.clear();
}

std::optional<PsoLayoutDesc> pso_lookup(ID3D12PipelineState* pso) {
    std::shared_lock lk(g_pso_mu);
    auto it = g_pso_map.find(pso);
    if (it != g_pso_map.end()) return it->second;
    return std::nullopt;
}

// ---- SRV map --------------------------------------------------------------

void srv_insert(SIZE_T cpu_handle_ptr, SrvEntry entry) {
    std::unique_lock lk(g_srv_mu);
    g_srv_map.insert_or_assign(cpu_handle_ptr, std::move(entry));
}

void srv_clear() {
    std::unique_lock lk(g_srv_mu);
    g_srv_map.clear();
}

std::optional<SrvEntry> srv_lookup(SIZE_T cpu_handle_ptr) {
    std::shared_lock lk(g_srv_mu);
    auto it = g_srv_map.find(cpu_handle_ptr);
    if (it != g_srv_map.end()) return it->second;
    return std::nullopt;
}

// ---- Per-CL state ---------------------------------------------------------

void cl_state_set(ID3D12GraphicsCommandList* cl, PerClState state) {
    std::unique_lock lk(g_cl_mu);
    g_cl_map.insert_or_assign(cl, std::move(state));
}

void cl_state_reset(ID3D12GraphicsCommandList* cl) {
    std::unique_lock lk(g_cl_mu);
    g_cl_map.erase(cl);
}

PerClState cl_state_get(ID3D12GraphicsCommandList* cl) {
    std::shared_lock lk(g_cl_mu);
    auto it = g_cl_map.find(cl);
    if (it != g_cl_map.end()) return it->second;
    return {};
}

// ---- Draw records ---------------------------------------------------------

void draw_record_push(DrawRecord rec) {
    std::lock_guard lk(g_draws_mu);
    g_draws.push_back(std::move(rec));
}

std::vector<DrawRecord> draw_records_take() {
    std::lock_guard lk(g_draws_mu);
    return std::exchange(g_draws, {});
}

void draw_record_release(DrawRecord& rec) {
    if (rec.pso) { rec.pso->Release(); rec.pso = nullptr; }
    for (UINT i = 0; i < k_max_vb_slots; ++i) {
        if (rec.vb_resources[i]) { rec.vb_resources[i]->Release(); rec.vb_resources[i] = nullptr; }
    }
    if (rec.ib_resource) { rec.ib_resource->Release(); rec.ib_resource = nullptr; }
    for (auto& [slot, res] : rec.textures) {
        if (res) res->Release();
    }
    rec.textures.clear();
}

} // namespace openripper::backends::d3d12
