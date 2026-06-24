#include "capture/dx11_resource_logger.h"
#include "core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#pragma comment(lib, "d3d11.lib")

namespace capture::dx11log {
namespace {
    using CreateTexture2DFn = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);

    using PSSetShaderResourcesFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
    using CSSetShaderResourcesFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
    using CSSetUnorderedAccessViewsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
    using OMSetRenderTargetsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    using OMSetRenderTargetsAndUAVsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
    using PSSetShaderFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
    using CSSetShaderFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
    using DrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
    using DrawIndexedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
    using DrawInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
    using DrawIndexedInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
    using DispatchFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);

    CreateTexture2DFn g_orig_create_tex2d = nullptr;
    PSSetShaderResourcesFn g_orig_ps_srvs = nullptr;
    CSSetShaderResourcesFn g_orig_cs_srvs = nullptr;
    CSSetUnorderedAccessViewsFn g_orig_cs_uavs = nullptr;
    OMSetRenderTargetsFn g_orig_om_rts = nullptr;
    OMSetRenderTargetsAndUAVsFn g_orig_om_rts_uavs = nullptr;
    PSSetShaderFn g_orig_ps_shader = nullptr;
    CSSetShaderFn g_orig_cs_shader = nullptr;
    DrawFn g_orig_draw = nullptr;
    DrawIndexedFn g_orig_draw_indexed = nullptr;
    DrawInstancedFn g_orig_draw_instanced = nullptr;
    DrawIndexedInstancedFn g_orig_draw_indexed_instanced = nullptr;
    DispatchFn g_orig_dispatch = nullptr;

    struct TexInfo {
        bool valid = false;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        UINT width = 0;
        UINT height = 0;
        UINT bind = 0;
        UINT mips = 0;
        UINT samples = 0;
        uintptr_t resource = 0;
    };

    std::mutex g_mtx;
    uint64_t g_draw_counter = 0;
    uint64_t g_dispatch_counter = 0;
    uint64_t g_taa_candidate_count = 0;
    uint64_t g_create_candidate_count = 0;
    ID3D11PixelShader* g_current_ps = nullptr;
    ID3D11ComputeShader* g_current_cs = nullptr;
    std::array<ID3D11ShaderResourceView*, 32> g_ps_srvs{};
    std::array<ID3D11ShaderResourceView*, 32> g_cs_srvs{};
    std::array<ID3D11RenderTargetView*, 8> g_rtvs{};
    ID3D11DepthStencilView* g_dsv = nullptr;
    std::array<ID3D11UnorderedAccessView*, 8> g_uavs{};
    std::unordered_set<uint64_t> g_seen_create_keys;
    std::unordered_set<uint64_t> g_seen_state_keys;
    std::unordered_set<uintptr_t> g_seen_ps;
    std::unordered_set<uintptr_t> g_seen_cs;

    const char* fmt_name(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
            case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
            case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
            case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
            case DXGI_FORMAT_R8G8_SNORM: return "R8G8_SNORM";
            case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
            case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
            case DXGI_FORMAT_R16G16_SNORM: return "R16G16_SNORM";
            case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
            case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
            case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
            case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
            case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
            case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
            case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
            case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
            case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
            case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
            case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
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

    bool hdr_color_like(DXGI_FORMAT f) {
        return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT;
    }

    bool ldr_color_like(DXGI_FORMAT f) {
        return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
               f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
               f == DXGI_FORMAT_R10G10B10A2_UNORM;
    }

    bool velocity_like(DXGI_FORMAT f) {
        return f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16_SNORM ||
               f == DXGI_FORMAT_R16G16_UNORM || f == DXGI_FORMAT_R32G32_FLOAT ||
               f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8_UNORM ||
               f == DXGI_FORMAT_R8G8_SNORM;
    }

    bool depth_srv_like(DXGI_FORMAT f) {
        return f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || f == DXGI_FORMAT_R32_FLOAT ||
               f == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS || f == DXGI_FORMAT_R16_UNORM;
    }

    bool depth_target_like(DXGI_FORMAT f) {
        return f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D24_UNORM_S8_UINT ||
               f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    }

    bool render_target_bind(UINT b) { return (b & D3D11_BIND_RENDER_TARGET) != 0; }
    bool depth_bind(UINT b) { return (b & D3D11_BIND_DEPTH_STENCIL) != 0; }
    bool srv_bind(UINT b) { return (b & D3D11_BIND_SHADER_RESOURCE) != 0; }
    bool uav_bind(UINT b) { return (b & D3D11_BIND_UNORDERED_ACCESS) != 0; }

    TexInfo tex_info_from_resource(ID3D11Resource* res, DXGI_FORMAT view_fmt = DXGI_FORMAT_UNKNOWN) {
        TexInfo out{};
        if (!res) return out;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex)
            return out;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        out.valid = true;
        out.fmt = view_fmt != DXGI_FORMAT_UNKNOWN ? view_fmt : td.Format;
        out.width = td.Width;
        out.height = td.Height;
        out.bind = td.BindFlags;
        out.mips = td.MipLevels;
        out.samples = td.SampleDesc.Count;
        out.resource = reinterpret_cast<uintptr_t>(res);
        tex->Release();
        return out;
    }

    TexInfo tex_info_from_srv(ID3D11ShaderResourceView* srv) {
        if (!srv) return {};
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        srv->GetDesc(&sd);
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        TexInfo ti = tex_info_from_resource(res, sd.Format);
        if (res) res->Release();
        return ti;
    }

    TexInfo tex_info_from_rtv(ID3D11RenderTargetView* rtv) {
        if (!rtv) return {};
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rtv->GetDesc(&rd);
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        TexInfo ti = tex_info_from_resource(res, rd.Format);
        if (res) res->Release();
        return ti;
    }

    TexInfo tex_info_from_dsv(ID3D11DepthStencilView* dsv) {
        if (!dsv) return {};
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dsv->GetDesc(&dd);
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        TexInfo ti = tex_info_from_resource(res, dd.Format);
        if (res) res->Release();
        return ti;
    }

    TexInfo tex_info_from_uav(ID3D11UnorderedAccessView* uav) {
        if (!uav) return {};
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        uav->GetDesc(&ud);
        ID3D11Resource* res = nullptr;
        uav->GetResource(&res);
        TexInfo ti = tex_info_from_resource(res, ud.Format);
        if (res) res->Release();
        return ti;
    }

    uint64_t compact_key(const TexInfo& ti, UINT slot = 0) {
        return (static_cast<uint64_t>(ti.resource >> 4) ^ (static_cast<uint64_t>(ti.fmt) << 48) ^
                (static_cast<uint64_t>(ti.width) << 24) ^ static_cast<uint64_t>(ti.height) ^
                (static_cast<uint64_t>(slot) << 56));
    }

    void release_srv_array(std::array<ID3D11ShaderResourceView*, 32>& arr, UINT start, UINT count) {
        for (UINT i = 0; i < count && start + i < arr.size(); ++i) {
            if (arr[start + i]) {
                arr[start + i]->Release();
                arr[start + i] = nullptr;
            }
        }
    }

    void retain_srv_array(std::array<ID3D11ShaderResourceView*, 32>& arr, UINT start, UINT count, ID3D11ShaderResourceView* const* srvs) {
        release_srv_array(arr, start, count);
        if (!srvs) return;
        for (UINT i = 0; i < count && start + i < arr.size(); ++i) {
            arr[start + i] = srvs[i];
            if (arr[start + i]) arr[start + i]->AddRef();
        }
    }

    void retain_rt_state(UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
        for (auto*& r : g_rtvs) {
            if (r) { r->Release(); r = nullptr; }
        }
        for (UINT i = 0; i < count && i < g_rtvs.size(); ++i) {
            g_rtvs[i] = rtvs ? rtvs[i] : nullptr;
            if (g_rtvs[i]) g_rtvs[i]->AddRef();
        }
        if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
        g_dsv = dsv;
        if (g_dsv) g_dsv->AddRef();
    }

    void retain_uav_state(UINT start, UINT count, ID3D11UnorderedAccessView* const* uavs) {
        if (start == D3D11_KEEP_UNORDERED_ACCESS_VIEWS) return;
        for (UINT i = 0; i < count && start < g_uavs.size() && start + i < g_uavs.size(); ++i) {
            if (g_uavs[start + i]) { g_uavs[start + i]->Release(); g_uavs[start + i] = nullptr; }
            g_uavs[start + i] = uavs ? uavs[i] : nullptr;
            if (g_uavs[start + i]) g_uavs[start + i]->AddRef();
        }
    }

    void log_tex(const char* prefix, UINT slot, const TexInfo& ti) {
        if (!ti.valid) return;
        LOGF("%s%u fmt=%s %ux%u bind=0x%X mips=%u samples=%u res=0x%p",
             prefix, slot, fmt_name(ti.fmt), ti.width, ti.height, ti.bind, ti.mips, ti.samples,
             reinterpret_cast<void*>(ti.resource));
    }

    int classify_score_srv(const TexInfo& ti, bool& has_hdr, bool& has_depth, bool& has_velocity, bool& has_history_color) {
        if (!ti.valid || !screenish(ti.width, ti.height)) return 0;
        int score = 0;
        if (hdr_color_like(ti.fmt)) { has_hdr = true; score += 4; }
        if (depth_srv_like(ti.fmt)) { has_depth = true; score += 4; }
        if (velocity_like(ti.fmt)) { has_velocity = true; score += 2; }
        if (ldr_color_like(ti.fmt)) { has_history_color = true; score += 1; }
        if (srv_bind(ti.bind)) score += 1;
        return score;
    }

    bool output_like(const TexInfo& ti) {
        if (!ti.valid || !screenish(ti.width, ti.height)) return false;
        return render_target_bind(ti.bind) && (ldr_color_like(ti.fmt) || hdr_color_like(ti.fmt));
    }

    void trace_current_state(const char* kind, UINT a = 0, UINT b = 0, UINT c = 0) {
        bool has_hdr = false, has_depth = false, has_velocity = false, has_history_color = false;
        int score = 0;
        for (UINT i = 0; i < 16; ++i) score += classify_score_srv(tex_info_from_srv(g_ps_srvs[i]), has_hdr, has_depth, has_velocity, has_history_color);
        for (UINT i = 0; i < 16; ++i) score += classify_score_srv(tex_info_from_srv(g_cs_srvs[i]), has_hdr, has_depth, has_velocity, has_history_color);

        bool has_output = false;
        for (UINT i = 0; i < 4; ++i) {
            TexInfo rt = tex_info_from_rtv(g_rtvs[i]);
            if (output_like(rt)) has_output = true;
        }
        for (UINT i = 0; i < 4; ++i) {
            TexInfo uav = tex_info_from_uav(g_uavs[i]);
            if (output_like(uav)) has_output = true;
        }

        TexInfo dsv = tex_info_from_dsv(g_dsv);
        if (dsv.valid && screenish(dsv.width, dsv.height) && depth_target_like(dsv.fmt)) has_depth = true;

        bool fullscreenish = false;
        if (kind[0] == 'D') {
            fullscreenish = (a == 3 || a == 4 || a == 6 || a <= 12 || b <= 12);
        } else {
            fullscreenish = true;
        }

        bool candidate = has_output && fullscreenish && ((has_hdr && has_depth) || (has_velocity && has_history_color) || score >= 7);
        if (!candidate) return;

        uint64_t state_key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_current_ps) >> 4) ^
                              (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_current_cs) >> 4) << 17) ^
                              (g_draw_counter << 28) ^ (g_dispatch_counter << 42));
        if (!g_seen_state_keys.insert(state_key).second && g_taa_candidate_count > 200) return;
        if (g_taa_candidate_count++ > 450) return;

        LOGF("[taa-trace] candidate #%llu kind=%s draw=%llu dispatch=%llu args=%u,%u,%u score=%d hdr=%d depth=%d velocityLike=%d historyColor=%d output=%d ps=0x%p cs=0x%p",
             static_cast<unsigned long long>(g_taa_candidate_count), kind,
             static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_dispatch_counter),
             a, b, c, score, has_hdr ? 1 : 0, has_depth ? 1 : 0, has_velocity ? 1 : 0,
             has_history_color ? 1 : 0, has_output ? 1 : 0, g_current_ps, g_current_cs);

        for (UINT i = 0; i < 4; ++i) log_tex("[taa-trace]   RTV", i, tex_info_from_rtv(g_rtvs[i]));
        log_tex("[taa-trace]   DSV", 0, dsv);
        for (UINT i = 0; i < 16; ++i) log_tex("[taa-trace]   PS_SRV", i, tex_info_from_srv(g_ps_srvs[i]));
        for (UINT i = 0; i < 16; ++i) log_tex("[taa-trace]   CS_SRV", i, tex_info_from_srv(g_cs_srvs[i]));
        for (UINT i = 0; i < 4; ++i) log_tex("[taa-trace]   UAV", i, tex_info_from_uav(g_uavs[i]));
    }

    HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
        HRESULT hr = g_orig_create_tex2d(dev, desc, init, out);
        if (SUCCEEDED(hr) && desc && out && *out && screenish(desc->Width, desc->Height)) {
            bool interesting = hdr_color_like(desc->Format) || ldr_color_like(desc->Format) || velocity_like(desc->Format) ||
                               depth_target_like(desc->Format) || depth_srv_like(desc->Format) ||
                               depth_bind(desc->BindFlags) || render_target_bind(desc->BindFlags) || uav_bind(desc->BindFlags);
            if (interesting) {
                TexInfo ti{};
                ti.valid = true; ti.fmt = desc->Format; ti.width = desc->Width; ti.height = desc->Height;
                ti.bind = desc->BindFlags; ti.mips = desc->MipLevels; ti.samples = desc->SampleDesc.Count;
                ti.resource = reinterpret_cast<uintptr_t>(*out);
                uint64_t key = compact_key(ti);
                std::lock_guard<std::mutex> lk(g_mtx);
                if (g_seen_create_keys.insert(key).second && g_create_candidate_count++ < 600) {
                    LOGF("[taa-trace] create2d #%llu fmt=%s %ux%u bind=0x%X mips=%u samples=%u usage=%u res=0x%p",
                         static_cast<unsigned long long>(g_create_candidate_count), fmt_name(desc->Format), desc->Width, desc->Height,
                         desc->BindFlags, desc->MipLevels, desc->SampleDesc.Count, desc->Usage, *out);
                }
            }
        }
        return hr;
    }

    void STDMETHODCALLTYPE hk_PSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT count, ID3D11ShaderResourceView* const* srvs) {
        { std::lock_guard<std::mutex> lk(g_mtx); retain_srv_array(g_ps_srvs, start, count, srvs); }
        g_orig_ps_srvs(ctx, start, count, srvs);
    }

    void STDMETHODCALLTYPE hk_CSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT count, ID3D11ShaderResourceView* const* srvs) {
        { std::lock_guard<std::mutex> lk(g_mtx); retain_srv_array(g_cs_srvs, start, count, srvs); }
        g_orig_cs_srvs(ctx, start, count, srvs);
    }

    void STDMETHODCALLTYPE hk_CSSetUnorderedAccessViews(ID3D11DeviceContext* ctx, UINT start, UINT count, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
        { std::lock_guard<std::mutex> lk(g_mtx); retain_uav_state(start, count, uavs); }
        g_orig_cs_uavs(ctx, start, count, uavs, counts);
    }

    void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
        { std::lock_guard<std::mutex> lk(g_mtx); retain_rt_state(count, rtvs, dsv); }
        g_orig_om_rts(ctx, count, rtvs, dsv);
    }

    void STDMETHODCALLTYPE hk_OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* ctx, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, UINT uav_start, UINT uav_count, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
        { std::lock_guard<std::mutex> lk(g_mtx); retain_rt_state(count, rtvs, dsv); retain_uav_state(uav_start, uav_count, uavs); }
        g_orig_om_rts_uavs(ctx, count, rtvs, dsv, uav_start, uav_count, uavs, counts);
    }

    void STDMETHODCALLTYPE hk_PSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* shader, ID3D11ClassInstance* const* classes, UINT class_count) {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_current_ps = shader;
            uintptr_t p = reinterpret_cast<uintptr_t>(shader);
            if (shader && g_seen_ps.insert(p).second && g_seen_ps.size() <= 256)
                LOGF("[taa-trace] PSSetShader ps=0x%p draw=%llu uniquePS=%llu", shader, static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_seen_ps.size()));
        }
        g_orig_ps_shader(ctx, shader, classes, class_count);
    }

    void STDMETHODCALLTYPE hk_CSSetShader(ID3D11DeviceContext* ctx, ID3D11ComputeShader* shader, ID3D11ClassInstance* const* classes, UINT class_count) {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_current_cs = shader;
            uintptr_t p = reinterpret_cast<uintptr_t>(shader);
            if (shader && g_seen_cs.insert(p).second && g_seen_cs.size() <= 128)
                LOGF("[taa-trace] CSSetShader cs=0x%p dispatch=%llu uniqueCS=%llu", shader, static_cast<unsigned long long>(g_dispatch_counter), static_cast<unsigned long long>(g_seen_cs.size()));
        }
        g_orig_cs_shader(ctx, shader, classes, class_count);
    }

    void note_draw_locked(const char* kind, UINT a, UINT b = 0, UINT c = 0) {
        ++g_draw_counter;
        if (g_draw_counter == 1 || g_draw_counter == 100 || g_draw_counter == 1000 || (g_draw_counter % 5000) == 0) {
            LOGF("[taa-trace] progress draw=%llu dispatch=%llu candidates=%llu creates=%llu",
                 static_cast<unsigned long long>(g_draw_counter), static_cast<unsigned long long>(g_dispatch_counter),
                 static_cast<unsigned long long>(g_taa_candidate_count), static_cast<unsigned long long>(g_create_candidate_count));
        }
        trace_current_state(kind, a, b, c);
    }

    void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext* ctx, UINT vertex_count, UINT start_vertex) {
        { std::lock_guard<std::mutex> lk(g_mtx); note_draw_locked("Draw", vertex_count, start_vertex, 0); }
        g_orig_draw(ctx, vertex_count, start_vertex);
    }

    void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext* ctx, UINT index_count, UINT start_index, INT base_vertex) {
        { std::lock_guard<std::mutex> lk(g_mtx); note_draw_locked("DrawIndexed", index_count, start_index, static_cast<UINT>(base_vertex)); }
        g_orig_draw_indexed(ctx, index_count, start_index, base_vertex);
    }

    void STDMETHODCALLTYPE hk_DrawInstanced(ID3D11DeviceContext* ctx, UINT vc, UINT ic, UINT sv, UINT si) {
        { std::lock_guard<std::mutex> lk(g_mtx); note_draw_locked("DrawInstanced", vc, ic, sv); }
        g_orig_draw_instanced(ctx, vc, ic, sv, si);
    }

    void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT icpi, UINT inst, UINT start, INT base, UINT start_inst) {
        { std::lock_guard<std::mutex> lk(g_mtx); note_draw_locked("DrawIndexedInstanced", icpi, inst, start); }
        g_orig_draw_indexed_instanced(ctx, icpi, inst, start, base, start_inst);
    }

    void STDMETHODCALLTYPE hk_Dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
        { std::lock_guard<std::mutex> lk(g_mtx); ++g_dispatch_counter; trace_current_state("Dispatch", x, y, z); }
        g_orig_dispatch(ctx, x, y, z);
    }

    void release_all_refs() {
        for (auto*& s : g_ps_srvs) { if (s) { s->Release(); s = nullptr; } }
        for (auto*& s : g_cs_srvs) { if (s) { s->Release(); s = nullptr; } }
        for (auto*& r : g_rtvs) { if (r) { r->Release(); r = nullptr; } }
        for (auto*& u : g_uavs) { if (u) { u->Release(); u = nullptr; } }
        if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
    }
}

bool install() {
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LOGF("[taa-trace] MH_Initialize failed mh=%d", (int)init);
        return false;
    }

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

    void** dvt = *reinterpret_cast<void***>(dev);
    void** cvt = *reinterpret_cast<void***>(ctx);

    struct HookSpec { void* target; void* detour; void** orig; const char* name; };
    HookSpec hooks[] = {
        { dvt[5],  reinterpret_cast<LPVOID>(&hk_CreateTexture2D), reinterpret_cast<void**>(&g_orig_create_tex2d), "CreateTexture2D" },
        { cvt[8],  reinterpret_cast<LPVOID>(&hk_PSSetShaderResources), reinterpret_cast<void**>(&g_orig_ps_srvs), "PSSetShaderResources" },
        { cvt[9],  reinterpret_cast<LPVOID>(&hk_PSSetShader), reinterpret_cast<void**>(&g_orig_ps_shader), "PSSetShader" },
        { cvt[12], reinterpret_cast<LPVOID>(&hk_DrawIndexed), reinterpret_cast<void**>(&g_orig_draw_indexed), "DrawIndexed" },
        { cvt[13], reinterpret_cast<LPVOID>(&hk_Draw), reinterpret_cast<void**>(&g_orig_draw), "Draw" },
        { cvt[20], reinterpret_cast<LPVOID>(&hk_DrawIndexedInstanced), reinterpret_cast<void**>(&g_orig_draw_indexed_instanced), "DrawIndexedInstanced" },
        { cvt[21], reinterpret_cast<LPVOID>(&hk_DrawInstanced), reinterpret_cast<void**>(&g_orig_draw_instanced), "DrawInstanced" },
        { cvt[33], reinterpret_cast<LPVOID>(&hk_OMSetRenderTargets), reinterpret_cast<void**>(&g_orig_om_rts), "OMSetRenderTargets" },
        { cvt[34], reinterpret_cast<LPVOID>(&hk_OMSetRenderTargetsAndUAVs), reinterpret_cast<void**>(&g_orig_om_rts_uavs), "OMSetRenderTargetsAndUAVs" },
        { cvt[41], reinterpret_cast<LPVOID>(&hk_Dispatch), reinterpret_cast<void**>(&g_orig_dispatch), "Dispatch" },
        { cvt[67], reinterpret_cast<LPVOID>(&hk_CSSetShaderResources), reinterpret_cast<void**>(&g_orig_cs_srvs), "CSSetShaderResources" },
        { cvt[68], reinterpret_cast<LPVOID>(&hk_CSSetUnorderedAccessViews), reinterpret_cast<void**>(&g_orig_cs_uavs), "CSSetUnorderedAccessViews" },
        { cvt[69], reinterpret_cast<LPVOID>(&hk_CSSetShader), reinterpret_cast<void**>(&g_orig_cs_shader), "CSSetShader" },
    };

    bool ok = true;
    for (const HookSpec& h : hooks) {
        MH_STATUS cs = MH_CreateHook(h.target, h.detour, h.orig);
        if (cs != MH_OK && cs != MH_ERROR_ALREADY_CREATED) {
            LOGF("[taa-trace] hook create failed %s mh=%d", h.name, (int)cs);
            ok = false;
            continue;
        }
        MH_STATUS es = MH_EnableHook(h.target);
        if (es != MH_OK && es != MH_ERROR_ENABLED) {
            LOGF("[taa-trace] hook enable failed %s mh=%d", h.name, (int)es);
            ok = false;
        }
    }

    ctx->Release();
    dev->Release();

    if (!ok) return false;
    LOGF("[taa-trace] installed: CreateTexture2D, OM RT/DSV/UAV, PS/CS SRV, Draw, Dispatch");
    LOGF("[taa-trace] purpose: find HE2 TemporalUpscaler/TAA inputs: scene color, depth, history, velocity/motion vectors");
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    release_all_refs();
    g_current_ps = nullptr;
    g_current_cs = nullptr;
    g_draw_counter = 0;
    g_dispatch_counter = 0;
    g_taa_candidate_count = 0;
    g_create_candidate_count = 0;
    g_seen_create_keys.clear();
    g_seen_state_keys.clear();
    g_seen_ps.clear();
    g_seen_cs.clear();
}

} // namespace capture::dx11log
