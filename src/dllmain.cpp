#include <windows.h>
#include <thread>
#include <string>
#include <cstdlib>

#include "proxy/dxgi_proxy.h"
#include "hooks/swapchain_hook.h"
#include "hooks/depth_hook.h"
#include "hooks/dx12_queue_capture.h"
#include "overlay/overlay.h"
#include "core/config.h"
#include "core/log.h"
#include "capture/dx11_resource_logger.h"

namespace core {
Config& config() {
    static Config c;
    static bool loaded_env = false;
    if (!loaded_env) {
        loaded_env = true;
        wchar_t v[16]{};
        if (GetEnvironmentVariableW(L"FSRINJ_DX11_PACING", v, 16) > 0)
            c.dx11_frame_pacing.store(_wtoi(v) != 0);
        if (GetEnvironmentVariableW(L"FSRINJ_DX11_MENU_IN_GEN", v, 16) > 0)
            c.dx11_overlay_in_generated.store(_wtoi(v) != 0);
    }
    return c;
}
}

static std::wstring dll_directory(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    auto pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

static void install_hooks_thread() {
    capture::dx11log::install();
    hooks::install_swapchain_hooks();
    depth::install();
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        core::log_init(dll_directory(self));
        core::log_line("[boot] fsr-injector starting");
        proxy::load_real_dxgi();
        std::thread(install_hooks_thread).detach();
    } else if (reason == DLL_PROCESS_DETACH) {
        overlay::shutdown();
        depth::shutdown();
        capture::dx11log::shutdown();
        hooks::dx12::shutdown();
        hooks::remove_swapchain_hooks();
        proxy::unload_real_dxgi();
        core::log_shutdown();
    }
    return TRUE;
}

extern "C" {

STDAPI CreateDXGIFactory(REFIID riid, void** pp) {
    HRESULT hr = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
        proxy::p_CreateDXGIFactory)(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp)
        hooks::dx12::install_factory_hooks(reinterpret_cast<IDXGIFactory*>(*pp));
    return hr;
}

STDAPI CreateDXGIFactory1(REFIID riid, void** pp) {
    HRESULT hr = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
        proxy::p_CreateDXGIFactory1)(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp)
        hooks::dx12::install_factory_hooks(reinterpret_cast<IDXGIFactory*>(*pp));
    return hr;
}

STDAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** pp) {
    HRESULT hr = reinterpret_cast<HRESULT(WINAPI*)(UINT, REFIID, void**)>(
        proxy::p_CreateDXGIFactory2)(flags, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp)
        hooks::dx12::install_factory_hooks(reinterpret_cast<IDXGIFactory*>(*pp));
    return hr;
}

STDAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** pp) {
    return reinterpret_cast<HRESULT(WINAPI*)(UINT, REFIID, void**)>(
        proxy::p_DXGIGetDebugInterface1)(flags, riid, pp);
}

STDAPI DXGIDeclareAdapterRemovalSupport() {
    return reinterpret_cast<HRESULT(WINAPI*)()>(
        proxy::p_DXGIDeclareAdapterRemovalSupport)();
}

}
