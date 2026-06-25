#include "capture/dx11_resource_logger.h"
#include "core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "d3d11.lib")

namespace capture::dx11log {
namespace {

using CreateTexture2DFn = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using OMSetRenderTargetsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUAVsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using PSSetShaderResourcesFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using CSSetShaderResourcesFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using PSSetShaderFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using CSSetShaderFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
using CSSetUnorderedAccessViewsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using DrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndexedInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DispatchFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using DispatchIndirectFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);

CreateTexture2DFn g_orig_create_tex2d = nullptr;
OMSetRenderTargetsFn g_orig_om_rt = nullptr;
OMSetRenderTargetsAndUAVsFn g_orig_om_rt_uav = nullptr;
PSSetShaderResourcesFn g_orig_ps_srvs = nullptr;
CSSetShaderResourcesFn g_orig_cs_srvs = nullptr;
PSSetShaderFn g_orig_ps_shader = nullptr;
CSSetShaderFn g_orig_cs_shader = nullptr;
CSSetUnorderedAccessViewsFn g_orig_cs_uavs = nullptr;
DrawFn g_orig_draw = nullptr;
DrawIndexedFn g_orig_draw_indexed = nullptr;
DrawInstancedFn g_orig_draw_instanced = nullptr;
DrawIndexedInstancedFn g_orig_draw_indexed_instanced = nullptr;
DispatchFn g_orig_dispatch = nullptr;
DispatchIndirectFn g_orig_dispatch_indirect = nullptr;

struct TexInfo {
    uint64_t id = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    UINT bind = 0;
    UINT mips = 0;
    UINT samples = 1;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
};

struct ViewInfo {
    ID3D11Resource* res = nullptr; // borrowed pointer only
    TexInfo tex{};
    DXGI_FORMAT view_fmt = DXGI_FORMAT_UNKNOWN;
};

struct BindState {
    std::array<ViewInfo, 8> rtvs{};
    ViewInfo dsv{};
    std::array<ViewInfo, 32> ps_srvs{};
    std::array<ViewInfo, 32> cs_srvs{};
    std::array<ViewInfo, 16> cs_uavs{};
    std::array<ViewInfo, 16> om_uavs{};
    ID3D11PixelShader* ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
};

struct ScoreInfo {
    bool has_output = false;
    bool has_hdr = false;
    bool has_depth = false;
    bool has_strong_velocity = false;
    bool has_weak_velocity = false;
    bool has_compute_output = false;
    int color_history_count = 0;
    int srv_count = 0;
    int uav_count = 0;
    int output_count = 0;
    int score = 0;
};

std::mutex g_mtx;
std::unordered_map<uintptr_t, TexInfo> g_textures;
std::unordered_set<uintptr_t> g_seen_ps;
std::unordered_set<uintptr_t> g_seen_cs;
std::unordered_set<uint64_t> g_seen_candidate_keys;
std::unordered_set<uint64_t> g_seen_snapshot_keys;
BindState g_state;
uint64_t g_next_tex_id = 1;
uint64_t g_draw_counter = 0;
uint64_t g_dispatch_counter = 0;
uint64_t g_event_counter = 0;
uint64_t g_candidate_counter = 0;
uint64_t g_snapshot_counter = 0;
uint64_t g_create_logged = 0;
uint64_t g_last_present_tick = 0;
LARGE_INTEGER g_qpc_freq{};
LARGE_INTEGER g_last_qpc{};

const char* fmt_name(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R8_UINT: return "R8_UINT";
        case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R8G8_SNORM: return "R8G8_SNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_SNORM: return "R8G8B8A8_SNORM";
        case DXGI_FORMAT_R8G8B8A8_UINT: return "R8G8B8A8_UINT";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
        case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
        case DXGI_FORMAT_R16_SNORM: return "R16_SNORM";
        case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
        case DXGI_FORMAT_R16G16_SNORM: return "R16G16_SNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R16G16B16A16_SNORM: return "R16G16B16A16_SNORM";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        default: return "OTHER";
    }
}

bool screenish(UINT w, UINT h) {
    if (w < 640 || h < 360) return false;
    float aspect = h ? static_cast<float>(w) / static_cast<float>(h) : 0.0f;
    return aspect >= 1.2f && aspect <= 2.5f;
}

bool hdr_like(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           f == DXGI_FORMAT_R11G11B10_FLOAT ||
           f == DXGI_FORMAT_R10G10B10A2_UNORM;
}

bool depth_view_like(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_D16_UNORM ||
           f == DXGI_FORMAT_D24_UNORM_S8_UINT ||
           f == DXGI_FORMAT_D32_FLOAT ||
           f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
           f == DXGI_FORMAT_R16_UNORM ||
           f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
           f == DXGI_FORMAT_R32_FLOAT ||
           f == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
}

DXGI_FORMAT effective_format(const TexInfo& tex, DXGI_FORMAT view_fmt) {
    return view_fmt != DXGI_FORMAT_UNKNOWN ? view_fmt : tex.fmt;
}

bool depth_resource_like(const TexInfo& t, DXGI_FORMAT view_fmt) {
    if (!screenish(t.width, t.height)) return false;
    if (t.bind & D3D11_BIND_DEPTH_STENCIL) return true;
    return depth_view_like(view_fmt) ||
           t.fmt == DXGI_FORMAT_R32_TYPELESS ||
           t.fmt == DXGI_FORMAT_R24G8_TYPELESS ||
           t.fmt == DXGI_FORMAT_R32G8X24_TYPELESS;
}

bool strong_velocity_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16_FLOAT ||
           f == DXGI_FORMAT_R16G16_SNORM ||
           f == DXGI_FORMAT_R16G16_UNORM ||
           f == DXGI_FORMAT_R32G32_FLOAT ||
           f == DXGI_FORMAT_R8G8_SNORM ||
           f == DXGI_FORMAT_R8G8_UNORM;
}

bool weak_velocity_or_packed_format(DXGI_FORMAT f) {
    return strong_velocity_format(f) ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_R8G8B8A8_SNORM ||
           f == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           f == DXGI_FORMAT_R16G16B16A16_SNORM;
}

bool color_history_like(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_R10G10B10A2_UNORM ||
           f == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           f == DXGI_FORMAT_R11G11B10_FLOAT;
}

TexInfo lookup_tex(ID3D11Resource* res) {
    TexInfo out{};
    if (!res) return out;
    auto it = g_textures.find(reinterpret_cast<uintptr_t>(res));
    if (it != g_textures.end()) return it->second;

    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        out.fmt = d.Format;
        out.width = d.Width;
        out.height = d.Height;
        out.bind = d.BindFlags;
        out.mips = d.MipLevels;
        out.samples = d.SampleDesc.Count;
        out.usage = d.Usage;
        tex->Release();
    }
    return out;
}

ViewInfo from_rtv(ID3D11RenderTargetView* rtv) {
    ViewInfo v{};
    if (!rtv) return v;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rtv->GetDesc(&rd);
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (res) {
        v.res = res;
        v.tex = lookup_tex(res);
        v.view_fmt = rd.Format;
        res->Release();
    }
    return v;
}

ViewInfo from_dsv(ID3D11DepthStencilView* dsv) {
    ViewInfo v{};
    if (!dsv) return v;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dsv->GetDesc(&dd);
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (res) {
        v.res = res;
        v.tex = lookup_tex(res);
        v.view_fmt = dd.Format;
        res->Release();
    }
    return v;
}

ViewInfo from_srv(ID3D11ShaderResourceView* srv) {
    ViewInfo v{};
    if (!srv) return v;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    srv->GetDesc(&sd);
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (res) {
        v.res = res;
        v.tex = lookup_tex(res);
        v.view_fmt = sd.Format;
        res->Release();
    }
    return v;
}

ViewInfo from_uav(ID3D11UnorderedAccessView* uav) {
    ViewInfo v{};
    if (!uav) return v;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    uav->GetDesc(&ud);
    ID3D11Resource* res = nullptr;
    uav->GetResource(&res);
    if (res) {
        v.res = res;
        v.tex = lookup_tex(res);
        v.view_fmt = ud.Format;
        res->Release();
    }
    return v;
}

void log_view(const char* prefix, int slot, const ViewInfo& v) {
    if (!v.res || !screenish(v.tex.width, v.tex.height)) return;
    DXGI_FORMAT fmt = effective_format(v.tex, v.view_fmt);
    LOGF("[taa-trace]   %s%d tex#%llu view=%s resfmt=%s %ux%u bind=0x%X mips=%u samples=%u res=0x%p",
         prefix, slot, static_cast<unsigned long long>(v.tex.id), fmt_name(fmt), fmt_name(v.tex.fmt),
         v.tex.width, v.tex.height, v.tex.bind, v.tex.mips, v.tex.samples, v.res);
}

void scan_input_view(ScoreInfo& s, const ViewInfo& v) {
    if (!v.res || !screenish(v.tex.width, v.tex.height)) return;
    ++s.srv_count;
    DXGI_FORMAT f = effective_format(v.tex, v.view_fmt);
    if (hdr_like(f)) { s.has_hdr = true; s.score += 3; }
    if (depth_resource_like(v.tex, f)) { s.has_depth = true; s.score += 3; }
    if (strong_velocity_format(f)) { s.has_strong_velocity = true; s.score += 5; }
    else if (weak_velocity_or_packed_format(f)) { s.has_weak_velocity = true; s.score += 1; }
    if (color_history_like(f)) { ++s.color_history_count; s.score += 1; }
}

void scan_output_view(ScoreInfo& s, const ViewInfo& v, bool compute_output) {
    if (!v.res || !screenish(v.tex.width, v.tex.height)) return;
    s.has_output = true;
    ++s.output_count;
    if (compute_output) { s.has_compute_output = true; ++s.uav_count; }
    DXGI_FORMAT f = effective_format(v.tex, v.view_fmt);
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || hdr_like(f)) s.score += 2;
}

ScoreInfo build_score_locked() {
    ScoreInfo si{};
    for (const auto& r : g_state.rtvs) scan_output_view(si, r, false);
    for (const auto& u : g_state.om_uavs) scan_output_view(si, u, true);
    for (const auto& u : g_state.cs_uavs) scan_output_view(si, u, true);
    if (depth_resource_like(g_state.dsv.tex, effective_format(g_state.dsv.tex, g_state.dsv.view_fmt))) {
        si.has_depth = true;
        si.score += 3;
    }
    for (const auto& s : g_state.ps_srvs) scan_input_view(si, s);
    for (const auto& s : g_state.cs_srvs) scan_input_view(si, s);
    return si;
}

uint64_t event_key(const char* kind, const ScoreInfo& s) {
    uintptr_t a = reinterpret_cast<uintptr_t>(g_state.ps);
    uintptr_t b = reinterpret_cast<uintptr_t>(g_state.cs);
    return (static_cast<uint64_t>(a >> 4) ^ static_cast<uint64_t>(b >> 3) ^
            (g_draw_counter << 11) ^ (g_dispatch_counter << 31) ^ static_cast<uint64_t>(s.score) ^
            (kind && kind[0] == 'D' ? 0xD15EA5EULL : 0xFACEB00CULL));
}

void log_bind_state(const char* prefix_all) {
    (void)prefix_all;
    for (int i = 0; i < static_cast<int>(g_state.rtvs.size()); ++i) log_view("RTV", i, g_state.rtvs[i]);
    for (int i = 0; i < static_cast<int>(g_state.om_uavs.size()); ++i) log_view("OM_UAV", i, g_state.om_uavs[i]);
    for (int i = 0; i < static_cast<int>(g_state.cs_uavs.size()); ++i) log_view("CS_UAV", i, g_state.cs_uavs[i]);
    log_view("DSV", 0, g_state.dsv);
    for (int i = 0; i < static_cast<int>(g_state.ps_srvs.size()); ++i) log_view("PS_SRV", i, g_state.ps_srvs[i]);
    for (int i = 0; i < static_cast<int>(g_state.cs_srvs.size()); ++i) log_view("CS_SRV", i, g_state.cs_srvs[i]);
}

void log_snapshot_locked(const char* kind, UINT a, UINT b, UINT c, const ScoreInfo& s, const char* reason) {
    if (g_snapshot_counter >= 90) return;
    if (s.score < 6) return;
    uint64_t key = event_key(kind, s) ^ 0x51504E415053484FULL;
    if (!g_seen_snapshot_keys.insert(key).second) return;
    ++g_snapshot_counter;
    LOGF("[frame-snapshot] #%llu reason=%s kind=%s event=%llu draw=%llu dispatch=%llu args=%u,%u,%u score=%d out=%d uavOut=%d hdr=%d depth=%d strongVel=%d weakVel=%d history=%d srvs=%d ps=0x%p cs=0x%p",
         static_cast<unsigned long long>(g_snapshot_counter), reason, kind,
         static_cast<unsigned long long>(g_event_counter),
         static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_dispatch_counter),
         a, b, c, s.score, s.output_count, s.uav_count, s.has_hdr ? 1 : 0, s.has_depth ? 1 : 0,
         s.has_strong_velocity ? 1 : 0, s.has_weak_velocity ? 1 : 0, s.color_history_count, s.srv_count,
         g_state.ps, g_state.cs);
    log_bind_state("snapshot");
}

void evaluate_candidate_locked(const char* kind, UINT a = 0, UINT b = 0, UINT c = 0) {
    ++g_event_counter;
    ScoreInfo si = build_score_locked();

    // v3 pseudo-capture: log a readable event list for rich compute/draw events even if the strict candidate rule misses them.
    bool dispatch_kind = kind && kind[0] == 'D';
    bool rich_compute = dispatch_kind && (si.has_compute_output || si.srv_count >= 2) && (si.has_hdr || si.has_depth || si.color_history_count >= 2 || si.has_weak_velocity || si.has_strong_velocity);
    bool rich_draw = !dispatch_kind && si.has_output && si.srv_count >= 2 && (si.has_hdr || si.has_depth || si.color_history_count >= 2);
    if (rich_compute) log_snapshot_locked(kind, a, b, c, si, "rich-compute-event");
    else if (rich_draw) log_snapshot_locked(kind, a, b, c, si, "rich-draw-event");

    bool is_interesting = false;
    // Classic draw path.
    if (si.has_output && si.has_hdr && si.has_depth && si.color_history_count >= 2 && (si.has_strong_velocity || si.has_weak_velocity)) is_interesting = true;
    // Strong velocity without explicit HDR can still be useful.
    if (si.has_output && si.has_depth && si.has_strong_velocity && si.color_history_count >= 1) is_interesting = true;
    // Compute path: often no RTV; output is UAV. Do not require depth because some temporal passes reconstruct it earlier.
    if (dispatch_kind && si.has_compute_output && si.has_hdr && si.color_history_count >= 2 && (si.has_depth || si.has_strong_velocity || si.has_weak_velocity)) is_interesting = true;
    // Compute path with depth + history, even if velocity is packed in a color buffer.
    if (dispatch_kind && si.has_compute_output && si.has_depth && si.color_history_count >= 2) is_interesting = true;

    if (!is_interesting) return;
    if (si.score < 9) return;
    if (g_candidate_counter >= 120) return;

    uint64_t key = event_key(kind, si);
    if (!g_seen_candidate_keys.insert(key).second) return;

    ++g_candidate_counter;
    LOGF("[taa-trace] candidate #%llu kind=%s event=%llu draw=%llu dispatch=%llu args=%u,%u,%u score=%d out=%d uavOut=%d hdr=%d depth=%d strongVelocity=%d weakVelocity=%d historyColor=%d srvs=%d ps=0x%p cs=0x%p",
         static_cast<unsigned long long>(g_candidate_counter), kind,
         static_cast<unsigned long long>(g_event_counter),
         static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_dispatch_counter),
         a, b, c, si.score, si.output_count, si.uav_count, si.has_hdr ? 1 : 0, si.has_depth ? 1 : 0,
         si.has_strong_velocity ? 1 : 0, si.has_weak_velocity ? 1 : 0,
         si.color_history_count, si.srv_count, g_state.ps, g_state.cs);
    log_bind_state("candidate");
}

HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out_tex) {
    HRESULT hr = g_orig_create_tex2d(dev, desc, init, out_tex);
    if (SUCCEEDED(hr) && desc && out_tex && *out_tex) {
        std::lock_guard<std::mutex> lk(g_mtx);
        TexInfo t{};
        t.id = g_next_tex_id++;
        t.fmt = desc->Format;
        t.width = desc->Width;
        t.height = desc->Height;
        t.bind = desc->BindFlags;
        t.mips = desc->MipLevels;
        t.samples = desc->SampleDesc.Count;
        t.usage = desc->Usage;
        g_textures[reinterpret_cast<uintptr_t>(*out_tex)] = t;

        bool interesting = screenish(t.width, t.height) &&
            (hdr_like(t.fmt) || weak_velocity_or_packed_format(t.fmt) ||
             (t.bind & D3D11_BIND_DEPTH_STENCIL) || (t.bind & D3D11_BIND_UNORDERED_ACCESS) || depth_resource_like(t, t.fmt));
        if (interesting && g_create_logged < 260) {
            ++g_create_logged;
            LOGF("[taa-trace] create2d #%llu fmt=%s %ux%u bind=0x%X mips=%u samples=%u usage=%u res=0x%p",
                 static_cast<unsigned long long>(t.id), fmt_name(t.fmt), t.width, t.height, t.bind,
                 t.mips, t.samples, static_cast<unsigned>(t.usage), *out_tex);
        }
    }
    return hr;
}

void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_state.rtvs = {};
        g_state.om_uavs = {};
        for (UINT i = 0; i < n && i < g_state.rtvs.size(); ++i) g_state.rtvs[i] = from_rtv(rtvs ? rtvs[i] : nullptr);
        g_state.dsv = from_dsv(dsv);
    }
    g_orig_om_rt(ctx, n, rtvs, dsv);
}

void STDMETHODCALLTYPE hk_OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, UINT uav_start, UINT uav_count, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
            g_state.rtvs = {};
            for (UINT i = 0; i < n && i < g_state.rtvs.size(); ++i) g_state.rtvs[i] = from_rtv(rtvs ? rtvs[i] : nullptr);
            g_state.dsv = from_dsv(dsv);
        }
        for (UINT i = 0; i < uav_count && (uav_start + i) < g_state.om_uavs.size(); ++i) {
            g_state.om_uavs[uav_start + i] = from_uav(uavs ? uavs[i] : nullptr);
        }
    }
    g_orig_om_rt_uav(ctx, n, rtvs, dsv, uav_start, uav_count, uavs, counts);
}

void STDMETHODCALLTYPE hk_PSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT count, ID3D11ShaderResourceView* const* srvs) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (UINT i = 0; i < count && (start + i) < g_state.ps_srvs.size(); ++i) {
            g_state.ps_srvs[start + i] = from_srv(srvs ? srvs[i] : nullptr);
        }
    }
    g_orig_ps_srvs(ctx, start, count, srvs);
}

void STDMETHODCALLTYPE hk_CSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT count, ID3D11ShaderResourceView* const* srvs) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (UINT i = 0; i < count && (start + i) < g_state.cs_srvs.size(); ++i) {
            g_state.cs_srvs[start + i] = from_srv(srvs ? srvs[i] : nullptr);
        }
    }
    g_orig_cs_srvs(ctx, start, count, srvs);
}

void STDMETHODCALLTYPE hk_CSSetUnorderedAccessViews(ID3D11DeviceContext* ctx, UINT start, UINT count, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (UINT i = 0; i < count && (start + i) < g_state.cs_uavs.size(); ++i) {
            g_state.cs_uavs[start + i] = from_uav(uavs ? uavs[i] : nullptr);
        }
    }
    g_orig_cs_uavs(ctx, start, count, uavs, counts);
}

void STDMETHODCALLTYPE hk_PSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* shader, ID3D11ClassInstance* const* classes, UINT class_count) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_state.ps = shader;
        uintptr_t p = reinterpret_cast<uintptr_t>(shader);
        if (shader && g_seen_ps.insert(p).second && g_seen_ps.size() <= 96) {
            LOGF("[taa-trace] PSSetShader ps=0x%p draw=%llu uniquePS=%llu", shader,
                 static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_seen_ps.size()));
        }
    }
    g_orig_ps_shader(ctx, shader, classes, class_count);
}

void STDMETHODCALLTYPE hk_CSSetShader(ID3D11DeviceContext* ctx, ID3D11ComputeShader* shader, ID3D11ClassInstance* const* classes, UINT class_count) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_state.cs = shader;
        uintptr_t p = reinterpret_cast<uintptr_t>(shader);
        if (shader && g_seen_cs.insert(p).second && g_seen_cs.size() <= 128) {
            LOGF("[taa-trace] CSSetShader cs=0x%p dispatch=%llu uniqueCS=%llu", shader,
                 static_cast<unsigned long long>(g_dispatch_counter), static_cast<unsigned long long>(g_seen_cs.size()));
        }
    }
    g_orig_cs_shader(ctx, shader, classes, class_count);
}

void note_draw_locked(UINT a, UINT b, UINT c, const char* kind) {
    ++g_draw_counter;
    evaluate_candidate_locked(kind, a, b, c);
    if (g_draw_counter == 1 || g_draw_counter == 1000 || (g_draw_counter % 5000) == 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        double ms = 0.0;
        if (g_qpc_freq.QuadPart && g_last_qpc.QuadPart) ms = (double)(now.QuadPart - g_last_qpc.QuadPart) * 1000.0 / (double)g_qpc_freq.QuadPart;
        g_last_qpc = now;
        LOGF("[taa-trace] progress draw=%llu dispatch=%llu events=%llu candidates=%llu snapshots=%llu creates=%llu trackedTextures=%llu dtMs=%.3f",
             static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_dispatch_counter),
             static_cast<unsigned long long>(g_event_counter), static_cast<unsigned long long>(g_candidate_counter),
             static_cast<unsigned long long>(g_snapshot_counter), static_cast<unsigned long long>(g_next_tex_id - 1),
             static_cast<unsigned long long>(g_textures.size()), ms);
    }
}

void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* ctx, UINT vc, UINT sv) {
    { std::lock_guard<std::mutex> lk(g_mtx); note_draw_locked(vc, sv, 0, "Draw"); }
    g_orig_draw(ctx, vc, sv);
}

void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* ctx, UINT ic, UINT si, INT bv) {
    { std::lock_guard<std::mutex> lk(g_mtx); note_draw_locked(ic, si, static_cast<UINT>(bv), "DrawIndexed"); }
    g_orig_draw_indexed(ctx, ic, si, bv);
}

void STDMETHODCALLTYPE hk_DrawInstanced(ID3D11DeviceContext* ctx, UINT vc, UINT inst, UINT sv, UINT si) {
    { std::lock_guard<std::mutex> lk(g_mtx); note_draw_locked(vc, inst, sv, "DrawInstanced"); }
    g_orig_draw_instanced(ctx, vc, inst, sv, si);
}

void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT icpi, UINT inst, UINT start, INT base, UINT start_inst) {
    { std::lock_guard<std::mutex> lk(g_mtx); note_draw_locked(icpi, inst, start, "DrawIndexedInstanced"); }
    g_orig_draw_indexed_instanced(ctx, icpi, inst, start, base, start_inst);
}

void STDMETHODCALLTYPE hk_Dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ++g_dispatch_counter;
        evaluate_candidate_locked("Dispatch", x, y, z);
        if (g_dispatch_counter == 1 || g_dispatch_counter == 100 || (g_dispatch_counter % 1000) == 0) {
            LOGF("[taa-trace] dispatch-progress draw=%llu dispatch=%llu events=%llu candidates=%llu snapshots=%llu creates=%llu trackedTextures=%llu",
                 static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_dispatch_counter),
                 static_cast<unsigned long long>(g_event_counter), static_cast<unsigned long long>(g_candidate_counter),
                 static_cast<unsigned long long>(g_snapshot_counter), static_cast<unsigned long long>(g_next_tex_id - 1),
                 static_cast<unsigned long long>(g_textures.size()));
        }
    }
    g_orig_dispatch(ctx, x, y, z);
}

void STDMETHODCALLTYPE hk_DispatchIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT offset) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ++g_dispatch_counter;
        evaluate_candidate_locked("DispatchIndirect", offset, 0, 0);
    }
    g_orig_dispatch_indirect(ctx, args, offset);
}

bool hook_one(void* target, void* detour, void** orig, const char* name) {
    MH_STATUS cs = MH_CreateHook(target, detour, orig);
    if (cs != MH_OK && cs != MH_ERROR_ALREADY_CREATED) {
        LOGF("[taa-trace] hook create failed %s mh=%d", name, static_cast<int>(cs));
        return false;
    }
    MH_STATUS es = MH_EnableHook(target);
    if (es != MH_OK && es != MH_ERROR_ENABLED) {
        LOGF("[taa-trace] hook enable failed %s mh=%d", name, static_cast<int>(es));
        return false;
    }
    return true;
}

} // namespace

bool install() {
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LOGF("[taa-trace] MH_Initialize failed mh=%d", static_cast<int>(init));
        return false;
    }

    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_last_qpc);

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr) || !dev || !ctx) {
        LOGF("[taa-trace] dummy device failed hr=0x%08lX", hr);
        if (ctx) ctx->Release();
        if (dev) dev->Release();
        return false;
    }

    void** dev_vt = *reinterpret_cast<void***>(dev);
    void** ctx_vt = *reinterpret_cast<void***>(ctx);

    bool ok = true;
    ok &= hook_one(dev_vt[5],  reinterpret_cast<void*>(&hk_CreateTexture2D), reinterpret_cast<void**>(&g_orig_create_tex2d), "CreateTexture2D");
    ok &= hook_one(ctx_vt[8],  reinterpret_cast<void*>(&hk_PSSetShaderResources), reinterpret_cast<void**>(&g_orig_ps_srvs), "PSSetShaderResources");
    ok &= hook_one(ctx_vt[9],  reinterpret_cast<void*>(&hk_PSSetShader), reinterpret_cast<void**>(&g_orig_ps_shader), "PSSetShader");
    ok &= hook_one(ctx_vt[12], reinterpret_cast<void*>(&hk_DrawIndexed), reinterpret_cast<void**>(&g_orig_draw_indexed), "DrawIndexed");
    ok &= hook_one(ctx_vt[13], reinterpret_cast<void*>(&hk_Draw), reinterpret_cast<void**>(&g_orig_draw), "Draw");
    ok &= hook_one(ctx_vt[20], reinterpret_cast<void*>(&hk_DrawIndexedInstanced), reinterpret_cast<void**>(&g_orig_draw_indexed_instanced), "DrawIndexedInstanced");
    ok &= hook_one(ctx_vt[21], reinterpret_cast<void*>(&hk_DrawInstanced), reinterpret_cast<void**>(&g_orig_draw_instanced), "DrawInstanced");
    ok &= hook_one(ctx_vt[33], reinterpret_cast<void*>(&hk_OMSetRenderTargets), reinterpret_cast<void**>(&g_orig_om_rt), "OMSetRenderTargets");
    ok &= hook_one(ctx_vt[34], reinterpret_cast<void*>(&hk_OMSetRenderTargetsAndUAVs), reinterpret_cast<void**>(&g_orig_om_rt_uav), "OMSetRenderTargetsAndUnorderedAccessViews");
    ok &= hook_one(ctx_vt[41], reinterpret_cast<void*>(&hk_Dispatch), reinterpret_cast<void**>(&g_orig_dispatch), "Dispatch");
    ok &= hook_one(ctx_vt[42], reinterpret_cast<void*>(&hk_DispatchIndirect), reinterpret_cast<void**>(&g_orig_dispatch_indirect), "DispatchIndirect");

    // Correct D3D11 immediate context indices for compute-stage binding.
    ok &= hook_one(ctx_vt[67], reinterpret_cast<void*>(&hk_CSSetShaderResources), reinterpret_cast<void**>(&g_orig_cs_srvs), "CSSetShaderResources");
    ok &= hook_one(ctx_vt[68], reinterpret_cast<void*>(&hk_CSSetUnorderedAccessViews), reinterpret_cast<void**>(&g_orig_cs_uavs), "CSSetUnorderedAccessViews");
    ok &= hook_one(ctx_vt[69], reinterpret_cast<void*>(&hk_CSSetShader), reinterpret_cast<void**>(&g_orig_cs_shader), "CSSetShader");

    ctx->Release();
    dev->Release();

    if (!ok) LOGF("[taa-trace] install incomplete; logger may be partial");

    LOGF("[taa-trace] v3 installed: TemporalUpscaler/TAA tracer + pseudo frame snapshot");
    LOGF("[taa-trace] v3 hooks CS SRV/UAV/shader with corrected D3D11 vtable indices 67/68/69");
    LOGF("[taa-trace] v3 logs [frame-snapshot] rich draw/compute events and [taa-trace] candidate stronger matches");
    LOGF("[taa-trace] purpose: identify HE2 TemporalUpscaler inputs: scene color, depth/stencil, history, velocity/motion vectors, compute UAV output");
    return ok;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_textures.clear();
    g_seen_ps.clear();
    g_seen_cs.clear();
    g_seen_candidate_keys.clear();
    g_seen_snapshot_keys.clear();
    g_state = {};
    g_next_tex_id = 1;
    g_draw_counter = 0;
    g_dispatch_counter = 0;
    g_event_counter = 0;
    g_candidate_counter = 0;
    g_snapshot_counter = 0;
    g_create_logged = 0;
    g_last_qpc = {};
}

} // namespace capture::dx11log
