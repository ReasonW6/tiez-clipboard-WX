// Real D3D11/DirectComposition surface copies, without HWNDs, targets, capture,
// or desktop access. The shim observes the real update before EndDraw; it never
// reopens a surface to read unspecified previous BeginDraw contents.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <winrt/base.h>
#include "../../src-tauri/native/glass_background_surface.h"
#include "../../src-tauri/native/glass_renderer.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using winrt::com_ptr;
enum class Fault { none, badOffset, beginFailure, endFailure };
struct Observation {
    UINT width = 0, height = 0, creates = 0, begins = 0, successfulBegins = 0, ends = 0;
    UINT forbidden = 0, subresource = 0, atlasWidth = 0, atlasHeight = 0;
    UINT destroyedSurfaces = 0;
    POINT actualOffset{};
    bool fullUpdate = false, sameDevice = false, releasedBeforeEnd = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_ALPHA_MODE alphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    Fault fault = Fault::none;
    com_ptr<ID3D11Texture2D> staging;
};
struct Result {
    const char* driver;
    size_t checks = 0, bytes = 0, copies = 0, nonzeroOffsets = 0;
    bool passed = false;
    std::string error;
};
void require(Result& result, bool condition, const char* message) {
    ++result.checks;
    if (!condition) throw std::runtime_error(message);
}
bool same_identity(IUnknown* a, IUnknown* b) {
    com_ptr<IUnknown> identityA, identityB;
    winrt::check_hresult(a->QueryInterface(IID_PPV_ARGS(identityA.put())));
    winrt::check_hresult(b->QueryInterface(IID_PPV_ARGS(identityB.put())));
    return identityA.get() == identityB.get();
}

// Only CreateSurface is allowed on this IDCompositionDevice. In particular the
// production helper must leave Commit, Wait and all visual bindings to its caller.
struct ObservedSurface final : IDCompositionSurface {
    ULONG references = 1;
    com_ptr<IDCompositionSurface> surface;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<ID3D11Texture2D> updateTexture;
    std::shared_ptr<Observation> observation;
    ObservedSurface(com_ptr<IDCompositionSurface> value, ID3D11DeviceContext* ctx,
        std::shared_ptr<Observation> state) : surface(std::move(value)), observation(std::move(state)) {
        context.copy_from(ctx);
    }
    ~ObservedSurface() { ++observation->destroyedSurfaces; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IDCompositionSurface)) return E_NOINTERFACE;
        *output = static_cast<IDCompositionSurface*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE BeginDraw(const RECT* rectangle, REFIID iid, void** object, POINT* offset) override {
        auto& state = *observation;
        ++state.begins;
        if (state.fault == Fault::beginFailure) return E_FAIL;
        const HRESULT started = surface->BeginDraw(rectangle, iid, object, offset);
        if (FAILED(started)) return started;
        ++state.successfulBegins;
        state.actualOffset = *offset;
        state.fullUpdate = !rectangle || (rectangle->left == 0 && rectangle->top == 0 &&
            rectangle->right == static_cast<LONG>(state.width) && rectangle->bottom == static_cast<LONG>(state.height));
        // The observer's extra references are relinquished before real EndDraw.
        // All production code still receives and uses the actual returned object.
        try {
            com_ptr<IDXGISurface2> dxgi;
            winrt::check_hresult(static_cast<IUnknown*>(*object)->QueryInterface(IID_PPV_ARGS(dxgi.put())));
            winrt::check_hresult(dxgi->GetResource(IID_PPV_ARGS(updateTexture.put()), &state.subresource));
            D3D11_TEXTURE2D_DESC desc{}; updateTexture->GetDesc(&desc);
            state.atlasWidth = desc.Width; state.atlasHeight = desc.Height;
            com_ptr<ID3D11Device> actualDevice, expectedDevice;
            updateTexture->GetDevice(actualDevice.put()); context->GetDevice(expectedDevice.put());
            state.sameDevice = same_identity(actualDevice.get(), expectedDevice.get());
            if (state.fault == Fault::badOffset) *offset = POINT{-1, -1};
            return S_OK;
        } catch (...) {
            // Failure in the test observer itself must not leak an active draw.
            updateTexture = nullptr;
            static_cast<IUnknown*>(*object)->Release(); *object = nullptr;
            surface->EndDraw();
            return winrt::to_hresult();
        }
    }
    HRESULT STDMETHODCALLTYPE EndDraw() override {
        auto& state = *observation;
        ++state.ends;
        HRESULT readbackStatus = S_OK;
        if (updateTexture && state.sameDevice && state.fault == Fault::none) {
            try {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = state.width; desc.Height = state.height;
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                com_ptr<ID3D11Device> device; context->GetDevice(device.put());
                winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, state.staging.put()));
                const UINT x = static_cast<UINT>(state.actualOffset.x), y = static_cast<UINT>(state.actualOffset.y);
                const D3D11_BOX box{x, y, 0, x + state.width, y + state.height, 1};
                context->CopySubresourceRegion(state.staging.get(), 0, 0, 0, 0,
                    updateTexture.get(), state.subresource, &box);
            } catch (...) { readbackStatus = winrt::to_hresult(); }
        }
        updateTexture = nullptr;
        state.releasedBeforeEnd = true;
        const HRESULT ended = surface->EndDraw();
        if (FAILED(ended)) return ended;
        if (FAILED(readbackStatus)) return readbackStatus;
        return state.fault == Fault::endFailure ? E_FAIL : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SuspendDraw() override { ++observation->forbidden; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResumeDraw() override { ++observation->forbidden; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Scroll(const RECT*, const RECT*, int, int) override { ++observation->forbidden; return E_NOTIMPL; }
};

struct ObservedDevice final : IDCompositionDevice {
    ULONG references = 1;
    com_ptr<IDCompositionDevice> device;
    com_ptr<ID3D11DeviceContext> context;
    std::shared_ptr<Observation> observation;
    ObservedDevice(IDCompositionDevice* value, ID3D11DeviceContext* ctx, std::shared_ptr<Observation> state)
        : observation(std::move(state)) { device.copy_from(value); context.copy_from(ctx); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IDCompositionDevice)) return E_NOINTERFACE;
        *output = static_cast<IDCompositionDevice*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE CreateSurface(UINT width, UINT height, DXGI_FORMAT format,
        DXGI_ALPHA_MODE alpha, IDCompositionSurface** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        auto& state = *observation;
        ++state.creates; state.width = width; state.height = height; state.format = format; state.alphaMode = alpha;
        com_ptr<IDCompositionSurface> real;
        const HRESULT status = device->CreateSurface(width, height, format, alpha, real.put());
        if (FAILED(status)) return status;
        *output = new ObservedSurface(std::move(real), context.get(), observation);
        return S_OK;
    }
#define FORBIDDEN_METHOD(name, arguments) HRESULT STDMETHODCALLTYPE name arguments override { ++observation->forbidden; return E_NOTIMPL; }
    FORBIDDEN_METHOD(Commit, ())
    FORBIDDEN_METHOD(WaitForCommitCompletion, ())
    FORBIDDEN_METHOD(GetFrameStatistics, (DCOMPOSITION_FRAME_STATISTICS*))
    FORBIDDEN_METHOD(CreateTargetForHwnd, (HWND, BOOL, IDCompositionTarget**))
    FORBIDDEN_METHOD(CreateVisual, (IDCompositionVisual**))
    FORBIDDEN_METHOD(CreateVirtualSurface, (UINT, UINT, DXGI_FORMAT, DXGI_ALPHA_MODE, IDCompositionVirtualSurface**))
    FORBIDDEN_METHOD(CreateSurfaceFromHandle, (HANDLE, IUnknown**))
    FORBIDDEN_METHOD(CreateSurfaceFromHwnd, (HWND, IUnknown**))
    FORBIDDEN_METHOD(CreateTranslateTransform, (IDCompositionTranslateTransform**))
    FORBIDDEN_METHOD(CreateScaleTransform, (IDCompositionScaleTransform**))
    FORBIDDEN_METHOD(CreateRotateTransform, (IDCompositionRotateTransform**))
    FORBIDDEN_METHOD(CreateSkewTransform, (IDCompositionSkewTransform**))
    FORBIDDEN_METHOD(CreateMatrixTransform, (IDCompositionMatrixTransform**))
    FORBIDDEN_METHOD(CreateTransformGroup, (IDCompositionTransform**, UINT, IDCompositionTransform**))
    FORBIDDEN_METHOD(CreateTranslateTransform3D, (IDCompositionTranslateTransform3D**))
    FORBIDDEN_METHOD(CreateScaleTransform3D, (IDCompositionScaleTransform3D**))
    FORBIDDEN_METHOD(CreateRotateTransform3D, (IDCompositionRotateTransform3D**))
    FORBIDDEN_METHOD(CreateMatrixTransform3D, (IDCompositionMatrixTransform3D**))
    FORBIDDEN_METHOD(CreateTransform3DGroup, (IDCompositionTransform3D**, UINT, IDCompositionTransform3D**))
    FORBIDDEN_METHOD(CreateEffectGroup, (IDCompositionEffectGroup**))
    FORBIDDEN_METHOD(CreateRectangleClip, (IDCompositionRectangleClip**))
    FORBIDDEN_METHOD(CreateAnimation, (IDCompositionAnimation**))
    FORBIDDEN_METHOD(CheckDeviceState, (BOOL*))
#undef FORBIDDEN_METHOD
};

struct DeviceSet {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDCompositionDevice> composition;
};
DeviceSet make_device(D3D_DRIVER_TYPE driver) {
    DeviceSet value;
    winrt::check_hresult(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, value.device.put(), nullptr, value.context.put()));
    const auto dxgi = value.device.as<IDXGIDevice>();
    winrt::check_hresult(DCompositionCreateDevice(dxgi.get(), IID_PPV_ARGS(value.composition.put())));
    return value;
}
com_ptr<IDCompositionDevice> observer(DeviceSet& devices, const std::shared_ptr<Observation>& state) {
    com_ptr<IDCompositionDevice> value;
    value.attach(new ObservedDevice(devices.composition.get(), devices.context.get(), state));
    return value;
}
D3D11_TEXTURE2D_DESC source_description(UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    return desc;
}
com_ptr<ID3D11Texture2D> texture(DeviceSet& devices, const D3D11_TEXTURE2D_DESC& desc,
    const std::vector<uint8_t>* bytes = nullptr) {
    com_ptr<ID3D11Texture2D> value;
    D3D11_SUBRESOURCE_DATA data{};
    if (bytes) { data.pSysMem = bytes->data(); data.SysMemPitch = desc.Width * 4; }
    winrt::check_hresult(devices.device->CreateTexture2D(&desc, bytes ? &data : nullptr, value.put()));
    return value;
}
template<class Action>
void must_throw(Result& result, Action action, const char* message) {
    bool threw = false;
    try { action(); } catch (const winrt::hresult_error&) { threw = true; }
    require(result, threw, message);
}
void successful_copy(Result& result, DeviceSet& devices, UINT width, UINT height, UINT seed,
    std::vector<com_ptr<IDCompositionSurface>>& retained) {
    // Spatially varying premultiplied BGRA with every alpha edge case. An offset,
    // stride, channel or whole-atlas mistake cannot pass a uniform-color check.
    std::vector<uint8_t> expected(static_cast<size_t>(width) * height * 4);
    constexpr std::array<UINT, 5> alpha{0, 1, 64, 128, 255};
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        const size_t index = (static_cast<size_t>(y) * width + x) * 4;
        const UINT a = alpha[(x + y + seed) % alpha.size()];
        expected[index] = static_cast<uint8_t>((x * 17 + y * 3 + seed) % (a + 1));
        expected[index + 1] = static_cast<uint8_t>((x * 5 + y * 11 + seed * 3) % (a + 1));
        expected[index + 2] = static_cast<uint8_t>((x * 7 + y * 19 + seed * 5) % (a + 1));
        expected[index + 3] = static_cast<uint8_t>(a);
    }
    auto source = texture(devices, source_description(width, height), &expected);
    auto state = std::make_shared<Observation>();
    auto shim = observer(devices, state);
    auto surface = tiez_glass_detail::make_background_surface(shim.get(), devices.context.get(), source.get());
    require(result, surface != nullptr, "helper returns a new initialized surface");
    require(result, state->creates == 1 && state->begins == 1 && state->successfulBegins == 1 && state->ends == 1,
        "successful surface has exactly one creation and paired BeginDraw/EndDraw");
    require(result, state->width == width && state->height == height && state->fullUpdate,
        "new surface is initialized across its complete source extent");
    require(result, state->format == DXGI_FORMAT_B8G8R8A8_UNORM && state->alphaMode == DXGI_ALPHA_MODE_PREMULTIPLIED,
        "surface format and premultiplied alpha are preserved");
    require(result, state->sameDevice && state->releasedBeforeEnd, "actual update uses the same D3D device and observer releases before EndDraw");
    require(result, state->forbidden == 0, "helper never commits, waits, binds, suspends or creates other composition objects");
    require(result, state->staging != nullptr, "test queued the actual update rectangle before EndDraw");
    winrt::check_hresult(devices.composition->Commit());
    winrt::check_hresult(devices.composition->WaitForCommitCompletion());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(devices.context->Map(state->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    size_t mismatches = 0;
    for (UINT y = 0; y < height; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < width * 4; ++x)
            mismatches += row[x] != expected[static_cast<size_t>(y) * width * 4 + x];
    }
    devices.context->Unmap(state->staging.get(), 0);
    require(result, mismatches == 0, "every actual atlas BGRA byte equals the full source including alpha");
    result.bytes += expected.size(); ++result.copies;
    if (state->actualOffset.x != 0 || state->actualOffset.y != 0) ++result.nonzeroOffsets;
    std::printf("%s copy %ux%u offset=(%ld,%ld) atlas=%ux%u subresource=%u bytes=%zu mismatches=%zu\n",
        result.driver, width, height, state->actualOffset.x, state->actualOffset.y,
        state->atlasWidth, state->atlasHeight, state->subresource, expected.size(), mismatches);
    // Keep several real surfaces alive to exercise nonzero atlas placements.
    retained.push_back(std::move(surface));
}

void opaque_tint_copy(Result& result, DeviceSet& devices, const std::array<float, 3>& color,
    const std::array<uint8_t, 4>& expected, std::vector<com_ptr<IDCompositionSurface>>& retained) {
    // Upload the production helper's packed word directly, as the renderer does.
    // The independently specified BGRA bytes catch a channel/order/alpha error
    // that a generated expected value copied from that same word would conceal.
    const uint32_t pixel = tiez_glass_detail::solid_tint_pixel(color[0], color[1], color[2]);
    auto desc = source_description(1, 1);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{&pixel, sizeof(pixel), sizeof(pixel)};
    com_ptr<ID3D11Texture2D> source;
    const HRESULT created = devices.device->CreateTexture2D(&desc, &data, source.put());
    std::printf("%s opaque source creation hr=0x%08lX usage=%u bind=%u\n", result.driver,
        static_cast<unsigned long>(created), static_cast<unsigned>(desc.Usage), desc.BindFlags);
    winrt::check_hresult(created);
    auto state = std::make_shared<Observation>();
    auto shim = observer(devices, state);
    auto surface = tiez_glass_detail::make_background_surface(shim.get(), devices.context.get(), source.get());
    source = nullptr;
    require(result, surface != nullptr && state->creates == 1 && state->successfulBegins == 1 && state->ends == 1,
        "opaque one-pixel tint creates one complete surface with a paired real draw");
    require(result, state->width == 1 && state->height == 1 && state->fullUpdate &&
        state->format == DXGI_FORMAT_B8G8R8A8_UNORM && state->alphaMode == DXGI_ALPHA_MODE_PREMULTIPLIED,
        "solid fallback copies one BGRA pixel rather than allocating an extent-sized texture");
    require(result, state->sameDevice && state->releasedBeforeEnd && state->staging != nullptr,
        "solid pixel copy uses the actual same-device atlas and releases update references before EndDraw");
    require(result, state->forbidden == 0,
        "solid source copy does not commit, bind, wait or alter any existing composition object");
    winrt::check_hresult(devices.composition->Commit());
    winrt::check_hresult(devices.composition->WaitForCommitCompletion());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(devices.context->Map(state->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::array<uint8_t, 4> actual{};
    std::copy_n(static_cast<const uint8_t*>(mapped.pData), actual.size(), actual.begin());
    devices.context->Unmap(state->staging.get(), 0);
    require(result, actual == expected,
        "all four atlas bytes exactly match the independent opaque tint oracle after source release");
    require(result, actual[3] == 255,
        "black, white and intermediate solid fallback colors all remain fully opaque");
    result.bytes += expected.size(); ++result.copies;
    if (state->actualOffset.x != 0 || state->actualOffset.y != 0) ++result.nonzeroOffsets;
    std::printf("%s opaque tint 0x%08lX offset=(%ld,%ld) atlas=%ux%u subresource=%u bytes=4\n",
        result.driver, static_cast<unsigned long>(pixel), state->actualOffset.x, state->actualOffset.y,
        state->atlasWidth, state->atlasHeight, state->subresource);
    retained.push_back(std::move(surface));
}

void invalid_inputs(Result& result, DeviceSet& devices, D3D_DRIVER_TYPE driver) {
    auto valid = texture(devices, source_description(32, 24));
    auto state = std::make_shared<Observation>(); auto shim = observer(devices, state);
    must_throw(result, [&] { tiez_glass_detail::make_background_surface(nullptr, devices.context.get(), valid.get()); }, "null composition device is rejected");
    must_throw(result, [&] { tiez_glass_detail::make_background_surface(shim.get(), nullptr, valid.get()); }, "null context is rejected");
    must_throw(result, [&] { tiez_glass_detail::make_background_surface(shim.get(), devices.context.get(), nullptr); }, "null source is rejected");
    auto invalid = [&](D3D11_TEXTURE2D_DESC desc, const char* message) {
        const auto source = texture(devices, desc);
        must_throw(result, [&] { tiez_glass_detail::make_background_surface(shim.get(), devices.context.get(), source.get()); }, message);
    };
    auto desc = source_description(32, 24); desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    invalid(desc, "non-BGRA source is rejected");
    desc = source_description(32, 24); desc.MipLevels = 2;
    invalid(desc, "multi-mip source is rejected");
    desc = source_description(32, 24); desc.ArraySize = 2;
    invalid(desc, "texture-array source is rejected");
    desc = source_description(32, 24); desc.SampleDesc.Count = 2;
    UINT quality = 0;
    winrt::check_hresult(devices.device->CheckMultisampleQualityLevels(desc.Format, 2, &quality));
    require(result, quality > 0, "test device supports the required real MSAA negative input");
    invalid(desc, "multisampled source is rejected");
    auto other = make_device(driver);
    must_throw(result, [&] { tiez_glass_detail::make_background_surface(shim.get(), other.context.get(), valid.get()); }, "source/context device mismatch is rejected");
    com_ptr<ID3D11DeviceContext> deferred;
    winrt::check_hresult(devices.device->CreateDeferredContext(0, deferred.put()));
    must_throw(result, [&] { tiez_glass_detail::make_background_surface(shim.get(), deferred.get(), valid.get()); }, "deferred context is rejected");
    require(result, state->creates == 0 && state->begins == 0 && state->ends == 0 && state->forbidden == 0,
        "invalid sources are rejected before any composition mutation");
    auto foreignState = std::make_shared<Observation>();
    com_ptr<IDCompositionDevice> foreignShim;
    foreignShim.attach(new ObservedDevice(other.composition.get(), devices.context.get(), foreignState));
    must_throw(result, [&] { tiez_glass_detail::make_background_surface(foreignShim.get(), devices.context.get(), valid.get()); },
        "a surface allocated on another D3D device is rejected");
    require(result, foreignState->begins == 1 && foreignState->successfulBegins == 1 && foreignState->ends == 1,
        "foreign atlas rejection still pairs the real BeginDraw with EndDraw");
    require(result, foreignState->forbidden == 0, "foreign atlas rejection performs no commit or binding");
    require(result, !foreignState->sameDevice && foreignState->staging == nullptr,
        "foreign atlas rejection cannot allocate staging or queue a cross-device readback");
}
void failures(Result& result, DeviceSet& devices) {
    auto source = texture(devices, source_description(17, 13));
    for (const auto fault : {Fault::badOffset, Fault::beginFailure, Fault::endFailure}) {
        auto state = std::make_shared<Observation>(); state->fault = fault;
        auto shim = observer(devices, state);
        must_throw(result, [&] { tiez_glass_detail::make_background_surface(shim.get(), devices.context.get(), source.get()); },
            "injected BeginDraw, atlas bounds or EndDraw failure is propagated");
        const UINT paired = fault == Fault::beginFailure ? 0u : 1u;
        require(result, state->begins == 1 && state->successfulBegins == paired && state->ends == paired,
            "draw failures end each successful BeginDraw exactly once, including a failing EndDraw");
        require(result, state->forbidden == 0, "exception path cannot commit or bind a candidate");
        // A subsequent real draw must succeed: an unpaired draw would leave the
        // DComp device in a drawing state and prevent this new surface update.
        auto recoveryState = std::make_shared<Observation>(); auto recoveryShim = observer(devices, recoveryState);
        auto recovered = tiez_glass_detail::make_background_surface(recoveryShim.get(), devices.context.get(), source.get());
        require(result, recovered != nullptr && recoveryState->successfulBegins == 1 && recoveryState->ends == 1,
            "device accepts a new real draw after the failure path");
    }
}
void cancelled_candidate(Result& result, DeviceSet& devices) {
    auto source = texture(devices, source_description(29, 19));
    auto oldState = std::make_shared<Observation>(); auto oldShim = observer(devices, oldState);
    auto oldSurface = tiez_glass_detail::make_background_surface(oldShim.get(), devices.context.get(), source.get());
    const POINT oldOffset = oldState->actualOffset;
    const UINT oldSubresource = oldState->subresource;
    auto state = std::make_shared<Observation>(); auto shim = observer(devices, state);
    tiez_glass_detail::PresentationEpoch epoch;
    const uint64_t snapshot = epoch.value;
    com_ptr<IDCompositionSurface> candidate;
    bool distinct = false, cancelled = false;
    UINT publishOperations = 0;
    const bool accepted = epoch.perform(snapshot, [&] {
        candidate = tiez_glass_detail::make_background_surface(shim.get(), devices.context.get(), source.get());
        distinct = !same_identity(candidate.get(), oldSurface.get());
        epoch.invalidate(); // Invalidation after EndDraw, before any publication.
    }, [&] { cancelled = true; candidate = nullptr; });
    if (accepted) ++publishOperations; // Simulated caller branch, no visual/target exists in this test.
    require(result, !accepted && cancelled && !candidate && publishOperations == 0,
        "production epoch fence cancels initialized stale candidate before publication branch");
    require(result, distinct && state->creates == 1 && state->successfulBegins == 1 && state->ends == 1 && state->destroyedSurfaces == 1,
        "stale candidate is a distinct fully initialized surface and is released");
    require(result, oldState->creates == 1 && oldState->successfulBegins == 1 && oldState->ends == 1 && oldState->destroyedSurfaces == 0 &&
        oldState->actualOffset.x == oldOffset.x && oldState->actualOffset.y == oldOffset.y && oldState->subresource == oldSubresource,
        "creating and cancelling a candidate never reopens or changes the old surface observation");
    require(result, oldState->forbidden == 0 && state->forbidden == 0,
        "cancelled helper and epoch path perform no Commit, wait, binding or other composition operation");
}
void run(Result& result, D3D_DRIVER_TYPE driver) {
    try {
        auto devices = make_device(driver);
        std::vector<com_ptr<IDCompositionSurface>> retained;
        const std::array<std::array<UINT, 2>, 6> sizes{{{1, 1}, {17, 13}, {61, 29}, {32, 64}, {257, 129}, {425, 589}}};
        UINT seed = 0;
        for (const auto& size : sizes) successful_copy(result, devices, size[0], size[1], ++seed, retained);
        opaque_tint_copy(result, devices, {0.0f, 0.0f, 0.0f}, {0, 0, 0, 255}, retained);
        opaque_tint_copy(result, devices, {1.0f, 1.0f, 1.0f}, {255, 255, 255, 255}, retained);
        opaque_tint_copy(result, devices, {1.0f, 0.0f, 0.0f}, {0, 0, 255, 255}, retained);
        opaque_tint_copy(result, devices, {0.0f, 1.0f, 0.0f}, {0, 255, 0, 255}, retained);
        opaque_tint_copy(result, devices, {0.0f, 0.0f, 1.0f}, {255, 0, 0, 255}, retained);
        opaque_tint_copy(result, devices, {0.1f, 0.2f, 0.3f}, {77, 51, 26, 255}, retained);
        opaque_tint_copy(result, devices, {0.5f, 0.5f, 0.5f}, {128, 128, 128, 255}, retained);
        require(result, result.nonzeroOffsets > 0, "real nonzero atlas placement was exercised");
        invalid_inputs(result, devices, driver);
        failures(result, devices);
        cancelled_candidate(result, devices);
        result.passed = true;
    } catch (const winrt::hresult_error& error) {
        char code[32]{}; std::snprintf(code, sizeof(code), "HRESULT 0x%08lX", static_cast<unsigned long>(error.code().value));
        result.error = code;
    } catch (const std::exception& error) { result.error = error.what(); }
    std::printf("%s %s checks=%zu copies=%zu bytes=%zu nonzero_offsets=%zu %s\n", result.driver,
        result.passed ? "PASS" : "FAIL", result.checks, result.copies, result.bytes,
        result.nonzeroOffsets, result.error.c_str());
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::fputs("usage: glass_background_surface_regression.exe report.json\n", stderr); return 2; }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 2;
    std::array<Result, 2> results{{{"hardware"}, {"warp"}}};
    run(results[0], D3D_DRIVER_TYPE_HARDWARE); run(results[1], D3D_DRIVER_TYPE_WARP);
    std::ofstream report{std::filesystem::path(argv[1])};
    report << "{\n  \"scope\": \"production surface helper; real D3D11/DComp; no HWND, target, capture or desktop access\",\n"
        "  \"readback\": \"queued by test shim before real EndDraw; Map only after caller Commit and Wait\",\n"
        "  \"limitations\": \"does not validate display timing, resize flicker or GPU completion at Wait return\",\n  \"drivers\": [\n";
    for (size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        report << "    {\"driver\": \"" << result.driver << "\", \"passed\": " << (result.passed ? "true" : "false")
            << ", \"checks\": " << result.checks << ", \"copies\": " << result.copies << ", \"comparedBytes\": " << result.bytes
            << ", \"nonzeroOffsetCopies\": " << result.nonzeroOffsets << ", \"error\": \"" << result.error << "\"}"
            << (index + 1 < results.size() ? ",\n" : "\n");
    }
    report << "  ]\n}\n"; report.close();
    CoUninitialize();
    return results[0].passed && results[1].passed ? 0 : 1;
}
