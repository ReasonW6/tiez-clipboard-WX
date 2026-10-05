#pragma once
#include <wrl.h>
#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <cwchar>

// Original, single-input effect description equivalent to material_effects.rs.
// The compositor consumes the Direct2D CLSID/properties; no Win2D runtime is used.
class ProbeGaussianBlur final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>,
    ABI::Windows::Graphics::Effects::IGraphicsEffect,
    ABI::Windows::Graphics::Effects::IGraphicsEffectSource,
    ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop>
{
    InspectableClass(L"TieZ.ProbeGaussianBlur", BaseTrust);
    winrt::hstring name_{L"Blur"};
    winrt::com_ptr<ABI::Windows::Graphics::Effects::IGraphicsEffectSource> source_;

public:
    explicit ProbeGaussianBlur(winrt::Windows::Graphics::Effects::IGraphicsEffectSource const& source)
        : source_(source.as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>()) {}

    HRESULT STDMETHODCALLTYPE get_Name(HSTRING* value) noexcept override {
        return value ? WindowsDuplicateString(static_cast<HSTRING>(winrt::get_abi(name_)), value) : E_POINTER;
    }
    HRESULT STDMETHODCALLTYPE put_Name(HSTRING value) noexcept override {
        try {
            UINT32 length = 0;
            const auto text = WindowsGetStringRawBuffer(value, &length);
            name_ = winrt::hstring(text, length);
            return S_OK;
        } catch (...) { return winrt::to_hresult(); }
    }
    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* value) noexcept override {
        if (!value) return E_POINTER;
        *value = CLSID_D2D1GaussianBlur;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR name, UINT* index,
        ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!name || !index || !mapping) return E_POINTER;
        if (std::wcscmp(name, L"BlurAmount") != 0) return E_INVALIDARG;
        *index = 0;
        *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 3;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        try {
            using winrt::Windows::Foundation::PropertyValue;
            winrt::Windows::Foundation::IInspectable property{nullptr};
            if (index == 0) property = PropertyValue::CreateSingle(4.0f);
            else if (index == 1 || index == 2) property = PropertyValue::CreateUInt32(1);
            else return E_INVALIDARG;
            *value = property.as<ABI::Windows::Foundation::IPropertyValue>().detach();
            return S_OK;
        } catch (...) { return winrt::to_hresult(); }
    }
    HRESULT STDMETHODCALLTYPE GetSource(UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (index != 0) return E_INVALIDARG;
        source_.copy_to(value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }
};
