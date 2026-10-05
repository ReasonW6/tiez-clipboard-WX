//! Effect descriptions consumed by the Windows compositor, without a Win2D runtime.
use windows::{
    core::{implement, Interface, Result, GUID, HSTRING, PCWSTR},
    Foundation::{IPropertyValue, PropertyValue},
    Graphics::Effects::{IGraphicsEffect, IGraphicsEffect_Impl, IGraphicsEffectSource, IGraphicsEffectSource_Impl},
    Win32::{Graphics::Direct2D::CLSID_D2D1GaussianBlur, System::WinRT::Graphics::Direct2D::{
        IGraphicsEffectD2D1Interop, IGraphicsEffectD2D1Interop_Impl,
        GRAPHICS_EFFECT_PROPERTY_MAPPING, GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT,
    }},
};

#[implement(IGraphicsEffect, IGraphicsEffectSource, IGraphicsEffectD2D1Interop)]
struct GaussianBlur { source: IGraphicsEffectSource, name: HSTRING, radius: f32 }

pub fn gaussian_blur(source: IGraphicsEffectSource) -> IGraphicsEffect {
    GaussianBlur { source, name: "Blur".into(), radius: 4.0 }.into()
}

impl IGraphicsEffectSource_Impl for GaussianBlur_Impl {}
impl IGraphicsEffect_Impl for GaussianBlur_Impl {
    fn Name(&self) -> Result<HSTRING> { Ok(self.name.clone()) }
    fn SetName(&self, _: &HSTRING) -> Result<()> { Ok(()) }
}
impl IGraphicsEffectD2D1Interop_Impl for GaussianBlur_Impl {
    fn GetEffectId(&self) -> Result<GUID> { Ok(CLSID_D2D1GaussianBlur) }
    fn GetNamedPropertyMapping(&self, name: &PCWSTR, index: *mut u32, mapping: *mut GRAPHICS_EFFECT_PROPERTY_MAPPING) -> Result<()> {
        if unsafe { name.to_string()? } != "BlurAmount" { return Err(windows::core::Error::from_hresult(windows::Win32::Foundation::E_INVALIDARG)); }
        unsafe { *index = 0; *mapping = GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT; }
        Ok(())
    }
    fn GetPropertyCount(&self) -> Result<u32> { Ok(3) }
    fn GetProperty(&self, index: u32) -> Result<IPropertyValue> {
        match index {
            0 => PropertyValue::CreateSingle(self.radius)?.cast(),
            1 => PropertyValue::CreateUInt32(1)?.cast(), // balanced optimization
            2 => PropertyValue::CreateUInt32(1)?.cast(), // hard border avoids dark seams
            _ => Err(windows::core::Error::from_hresult(windows::Win32::Foundation::E_INVALIDARG)),
        }
    }
    fn GetSource(&self, index: u32) -> Result<IGraphicsEffectSource> {
        if index == 0 { Ok(self.source.clone()) } else { Err(windows::core::Error::from_hresult(windows::Win32::Foundation::E_INVALIDARG)) }
    }
    fn GetSourceCount(&self) -> Result<u32> { Ok(1) }
}
