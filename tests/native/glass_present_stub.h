#pragma once
// Inject DXGI backpressure into the production submission state machine.
class PresentStub final : public IDXGISwapChain1 {
public:
    HRESULT result = S_OK;
    UINT interval = UINT_MAX, flags = 0, calls = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** value) override { *value=nullptr;return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID,void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID,void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT options) override { interval=sync;flags=options;++calls;return result; }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT,REFIID,void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL,IDXGIOutput*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*,IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT,UINT,UINT,DXGI_FORMAT,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID,void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present1(UINT,UINT,const DXGI_PRESENT_PARAMETERS*) override { return E_NOTIMPL; }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION*) override { return E_NOTIMPL; }
};
