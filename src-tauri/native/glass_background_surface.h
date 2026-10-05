#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <winrt/base.h>
#include <cstdint>
#include <limits>

namespace tiez_glass_detail {

// The caller owns source validity and epoch/scope checks. This creates a fully
// initialized candidate only; it never changes a visual or commits the device.
inline winrt::com_ptr<IDCompositionSurface> make_background_surface(
    IDCompositionDevice* composition, ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    if (!composition || !context || !source) throw winrt::hresult_invalid_argument();
    winrt::com_ptr<IDCompositionDevice> owner;
    winrt::com_ptr<ID3D11DeviceContext> drawingContext;
    winrt::com_ptr<ID3D11Texture2D> sourceTexture;
    owner.copy_from(composition);
    drawingContext.copy_from(context);
    sourceTexture.copy_from(source);
    if (drawingContext->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        throw winrt::hresult_invalid_argument();

    D3D11_TEXTURE2D_DESC sourceDescription{};
    sourceTexture->GetDesc(&sourceDescription);
    constexpr auto coordinateLimit = static_cast<UINT>((std::numeric_limits<LONG>::max)());
    if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        sourceDescription.MipLevels != 1 || sourceDescription.ArraySize != 1 ||
        sourceDescription.SampleDesc.Count != 1 || sourceDescription.SampleDesc.Quality != 0 ||
        sourceDescription.Width == 0 || sourceDescription.Height == 0 ||
        sourceDescription.Width > coordinateLimit || sourceDescription.Height > coordinateLimit)
        throw winrt::hresult_invalid_argument();

    winrt::com_ptr<ID3D11Device> sourceDevice, contextDevice;
    sourceTexture->GetDevice(sourceDevice.put());
    drawingContext->GetDevice(contextDevice.put());
    if (!sourceDevice || !contextDevice ||
        sourceDevice.as<IUnknown>() != contextDevice.as<IUnknown>())
        throw winrt::hresult_invalid_argument();

    winrt::com_ptr<IDCompositionSurface> candidate;
    winrt::check_hresult(owner->CreateSurface(sourceDescription.Width, sourceDescription.Height,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, candidate.put()));
    winrt::com_ptr<IDXGISurface> update;
    winrt::com_ptr<IDXGISurface2> update2;
    winrt::com_ptr<ID3D11Texture2D> destination;
    bool drawing = false;
    try {
        POINT offset{};
        winrt::check_hresult(candidate->BeginDraw(nullptr, __uuidof(IDXGISurface), update.put_void(), &offset));
        drawing = true;
        update2 = update.as<IDXGISurface2>();
        UINT subresource = 0;
        winrt::check_hresult(update2->GetResource(__uuidof(ID3D11Texture2D), destination.put_void(), &subresource));
        winrt::com_ptr<ID3D11Device> destinationDevice;
        destination->GetDevice(destinationDevice.put());
        if (!destinationDevice || destinationDevice.as<IUnknown>() != contextDevice.as<IUnknown>())
            throw winrt::hresult_invalid_argument();

        D3D11_TEXTURE2D_DESC destinationDescription{};
        destination->GetDesc(&destinationDescription);
        if (destinationDescription.Format != sourceDescription.Format ||
            destinationDescription.SampleDesc.Count != 1 || destinationDescription.SampleDesc.Quality != 0 ||
            destinationDescription.MipLevels == 0 || destinationDescription.ArraySize == 0 ||
            static_cast<uint64_t>(subresource) >= static_cast<uint64_t>(destinationDescription.MipLevels) * destinationDescription.ArraySize ||
            offset.x < 0 || offset.y < 0)
            throw winrt::hresult_invalid_argument();
        const UINT mip = subresource % destinationDescription.MipLevels;
        if (mip >= 32) throw winrt::hresult_invalid_argument();
        const UINT mipWidth = (destinationDescription.Width >> mip) ? destinationDescription.Width >> mip : 1;
        const UINT mipHeight = (destinationDescription.Height >> mip) ? destinationDescription.Height >> mip : 1;
        if (static_cast<uint64_t>(offset.x) + sourceDescription.Width > mipWidth ||
            static_cast<uint64_t>(offset.y) + sourceDescription.Height > mipHeight)
            throw winrt::hresult_invalid_argument();

        const D3D11_BOX wholeSource{0, 0, 0, sourceDescription.Width, sourceDescription.Height, 1};
        drawingContext->CopySubresourceRegion(destination.get(), subresource,
            static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0, sourceTexture.get(), 0, &wholeSource);
        destination = nullptr;
        update2 = nullptr;
        update = nullptr;
        drawing = false;
        winrt::check_hresult(candidate->EndDraw());
    } catch (...) {
        destination = nullptr;
        update2 = nullptr;
        update = nullptr;
        // A successful BeginDraw must be paired even when validation/copy setup
        // fails. The candidate stays unbound, so its pending pixels are harmless.
        if (drawing) candidate->EndDraw();
        throw;
    }
    return candidate;
}

} // namespace tiez_glass_detail
