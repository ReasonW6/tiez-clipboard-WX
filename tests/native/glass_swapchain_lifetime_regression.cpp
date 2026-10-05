// Hardware D3D11/DirectComposition lifetime regression, without any HWND,
// composition target, capture session, or desktop pixel access.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <cstdio>
#include <initializer_list>
using Microsoft::WRL::ComPtr;
const GUID markerId{0x0e17c588,0x2d43,0x4ae9,{0xb7,0xbc,0xf5,0x27,0x8f,0x94,0xa2,0xe1}};
struct Marker final : IUnknown {
    ULONG references = 1;
    bool* destroyed;
    explicit Marker(bool& flag) : destroyed(&flag) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *result = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references;
        if (!remaining) { *destroyed = true; delete this; }
        return remaining;
    }
};
#define CHECK(x) do { const HRESULT result=(x); if(FAILED(result)){std::printf("FAILED %s hr=%08lX\n",#x,result);return 2;} } while(0)
int main() {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    CHECK(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<IDXGIDevice> dxgi; CHECK(device.As(&dxgi));
    ComPtr<IDCompositionDesktopDevice> composition;
    CHECK(DCompositionCreateDevice3(dxgi.Get(),IID_PPV_ARGS(&composition)));
    ComPtr<IDCompositionVisual2> visual; CHECK(composition->CreateVisual(&visual));
    ComPtr<IDXGIAdapter> adapter; CHECK(dxgi->GetAdapter(&adapter));
    ComPtr<IDXGIFactory2> factory; CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)));
    for(bool retain : {false,true}) {
        bool destroyed=false;
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=desc.Height=32;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        desc.BufferCount=2;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Scaling=DXGI_SCALING_STRETCH;
        ComPtr<IDXGISwapChain1> swap;CHECK(factory->CreateSwapChainForComposition(device.Get(),&desc,nullptr,&swap));
        ComPtr<IUnknown> marker;marker.Attach(new Marker(destroyed));
        CHECK(swap->SetPrivateDataInterface(markerId,marker.Get()));marker.Reset();
        CHECK(visual->SetContent(swap.Get()));CHECK(composition->Commit());CHECK(composition->WaitForCommitCompletion());
        ComPtr<IDXGISwapChain1> previous;
        if(retain)previous=swap;
        swap.Reset();
        std::printf("retain_explicit_previous=%d after_member_replacement_swap_destroyed=%d\n",retain,destroyed);
        if (retain && destroyed) { std::puts("FAIL: mounted chain died during replacement"); return 1; }
        ComPtr<IDXGISwapChain1> replacement;
        CHECK(factory->CreateSwapChainForComposition(device.Get(),&desc,nullptr,&replacement));
        CHECK(visual->SetContent(replacement.Get()));CHECK(composition->Commit());CHECK(composition->WaitForCommitCompletion());
        std::printf("retain_explicit_previous=%d after_replacement_commit_swap_destroyed=%d\n",retain,destroyed);
        if (retain && destroyed) { std::puts("FAIL: previous chain did not survive completed replacement commit"); return 1; }
        previous.Reset();
        std::printf("retain_explicit_previous=%d after_explicit_previous_release_swap_destroyed=%d\n",retain,destroyed);
        if (!destroyed) { std::puts("FAIL: completed replacement leaked the old swap chain"); return 1; }
        CHECK(visual->SetContent(nullptr));CHECK(composition->Commit());CHECK(composition->WaitForCommitCompletion());
    }
    std::puts("PASS: previous swap survives until replacement commit, then releases. No HWND or desktop read.");
    return 0;
}
