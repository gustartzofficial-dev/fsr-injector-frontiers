#include <windows.h>
#include <string>
#include <cstdlib>

#include "proxy/dxgi_proxy.h"
#include "core/log.h"
#include "core/config.h"

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

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        core::log_init(dll_directory(self));
        core::log_line("[boot] dxgi smoke test loaded");
        proxy::load_real_dxgi();
    } else if (reason == DLL_PROCESS_DETACH) {
        proxy::unload_real_dxgi();
        core::log_shutdown();
    }
    return TRUE;
}

extern "C" {

STDAPI CreateDXGIFactory(REFIID riid, void** pp) {
    core::log_line("[dxgi] CreateDXGIFactory");
    return reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
        proxy::p_CreateDXGIFactory)(riid, pp);
}

STDAPI CreateDXGIFactory1(REFIID riid, void** pp) {
    core::log_line("[dxgi] CreateDXGIFactory1");
    return reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
        proxy::p_CreateDXGIFactory1)(riid, pp);
}

STDAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** pp) {
    core::log_line("[dxgi] CreateDXGIFactory2");
    return reinterpret_cast<HRESULT(WINAPI*)(UINT, REFIID, void**)>(
        proxy::p_CreateDXGIFactory2)(flags, riid, pp);
}

STDAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** pp) {
    core::log_line("[dxgi] DXGIGetDebugInterface1");
    return reinterpret_cast<HRESULT(WINAPI*)(UINT, REFIID, void**)>(
        proxy::p_DXGIGetDebugInterface1)(flags, riid, pp);
}

STDAPI DXGIDeclareAdapterRemovalSupport() {
    core::log_line("[dxgi] DXGIDeclareAdapterRemovalSupport");
    return reinterpret_cast<HRESULT(WINAPI*)()>(
        proxy::p_DXGIDeclareAdapterRemovalSupport)();
}

}
