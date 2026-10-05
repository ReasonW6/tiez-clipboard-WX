#pragma once

// Included after MaterialGpu and shared measurements. The oracle shader replaces
// only the production LOD call with a fixture-supplied constant; every blur pass,
// texture, offset, reflection, track, alpha and coverage operation still runs.
namespace surface_filter_regression {

struct FilterCase { float dpi, opacity, clearLod, regularLod, readingLod; };
// Fixed numerical expectations for the public clarity/DPI/material contract.
// No control size, radius, edge depth or sample position enters this table.
constexpr FilterCase Cases[] = {
    {1,0,0,2.792481250f,3.792481250f}, {1,50,.584962501f,4.921764267f,4.921764267f}, {1,100,1,5.792481250f,5.792481250f},
    {1.5f,0,0,3.377443751f,4.377443751f}, {1.5f,50,.807354922f,5.506726768f,5.506726768f}, {1.5f,100,1.321928095f,6.377443751f,6.377443751f},
    {2,0,0,3.792481250f,4.792481250f}, {2,50,1,5.921764267f,5.921764267f}, {2,100,1.584962501f,6.792481250f,6.792481250f}
};

Pixels multiscaleSource(unsigned phase) {
    Pixels pixels(Width * Height);
    for (unsigned y=0;y<Height;++y) for(unsigned x=0;x<Width;++x) {
        // Independent spatial frequencies distinguish a real mip footprint
        // from tint changes or an accidentally uniform source at one phase.
        const float value=.5f+.26f*std::sin((x+phase*7)*.137f)
            +.16f*std::sin((y+phase*11)*.311f)+((x+y)%2 ? .08f : -.08f);
        pixels[y*Width+x]={value,value,value,1};
    }
    return pixels;
}

} // namespace surface_filter_regression

void run_surface_filter_regression(MaterialGpu& current, MaterialGpu& constantLod,
    MaterialGpu& depthFilter, Report& report) {
    using namespace surface_filter_regression;
    constexpr float Byte=1.01f/255;
    const Control controls[] = {
        {{24,60,208,40},{20,0,0,0}}, {{24,24,208,112},{16,0,0,0}},
        {{113,69,30,22},{11,0,0,0}}, {{88,40,80,80},{32,0,0,0}}
    };
    for(unsigned geometry=0;geometry<4;++geometry) for(bool dark:{false,true})
    for(const auto& test:Cases) for(unsigned material:{0u,1u,2u}) {
        auto state=materialState(controls[geometry],dark,test.opacity);
        state.options[0]*=test.dpi; state.options[3]=test.dpi;
        state.controls[0].shape[2]=float(material);
        state.controls[0].shape[3]=material==2?test.readingLod:material==1?test.regularLod:test.clearLod;
        const auto source=multiscaleSource(geometry);
        const auto actual=current.material(source,state), expected=constantLod.material(source,state);
        const auto negative=depthFilter.material(source,state);
        const auto lod=current.surfaceLod(state);
        float lodError=0,edgeError=0,interiorError=0,outsideError=0,negativeError=0;
        unsigned edgeSamples=0,interiorSamples=0;
        for(unsigned y=0;y<Height;++y) for(unsigned x=0;x<Width;++x) {
            const auto i=y*Width+x; const float depth=interiorDepth(x,y,state.controls[0]);
            lodError=std::max(lodError,std::abs(lod[i][0]-state.controls[0].shape[3]));
            if(depth>=1 && depth<=.25f*state.controls[0].shape[0]) {
                ++edgeSamples; edgeError=std::max(edgeError,delta(actual.output[i],expected.output[i],4));
            }
            if(depth>=.65f*state.controls[0].shape[0]) {
                ++interiorSamples; interiorError=std::max(interiorError,delta(actual.output[i],expected.output[i],4));
            }
            if(depth<-.5f) outsideError=std::max(outsideError,delta(actual.output[i],actual.base[i],4));
            if(depth>=1) negativeError=std::max(negativeError,delta(negative.output[i],expected.output[i],4));
        }
        const std::string name="surface_g"+std::to_string(geometry)+(dark?"_dark":"_light")
            +"_dpi"+std::to_string(test.dpi)+"_opacity"+std::to_string(int(test.opacity))+(material==2?"_reading":material==1?"_regular":"_clear");
        report.check(name+"_contract_lod_float_error",lodError,.000002);
        report.check(name+"_edge_roi_is_nonempty",edgeSamples,20,true);
        report.check(name+"_interior_roi_is_nonempty",interiorSamples,20,true);
        report.check(name+"_complete_frame_matches_constant_footprint_oracle",maxDelta(actual.output,expected.output,4),Byte);
        report.check(name+"_edge_matches_same_footprint",edgeError,Byte);
        report.check(name+"_interior_matches_same_footprint",interiorError,Byte);
        report.check(name+"_depth_varying_filter_fails_same_bitmap_gate",negativeError,2*Byte,true);
        report.check(name+"_outside_preserves_window_base",outsideError,0);
        report.check(name+"_alpha_is_one",alphaError(actual.output),0);
        if(material==0 && test.opacity==0)
            report.check(name+"_maximum_clarity_preserves_raw_level_zero",maxDelta(actual.output,current.material(source,state,2).output,4),0);
    }
    // Repeated source patches isolate explicit surface material from source
    // position and geometry. Both controls have identical dimensions/radius.
    for(bool dark:{false,true}) for(float opacity:{0.f,50.f}) {
        auto source=detailSource(true,0);
        for(unsigned y=0;y<Height;++y)for(unsigned x=128;x<Width;++x)
            source[y*Width+x]=source[y*Width+x-120];
        auto state=materialState(Control{{16,24,96,112},{16,0,1,0}},dark,opacity);
        state.controls[1]={{136,24,96,112},{16,0,0,0}};state.options[1]=2;
        const auto frame=current.material(source,state).output;
        const auto unfiltered=current.material(source,state,2).output;
        const auto name=std::string("surface_pair_")+(dark?"dark_":"light_")+std::to_string(int(opacity));
        report.check(name+"_regular_mips_reduce_fine_detail",centerContrast(unfiltered,64)-centerContrast(frame,64),12,true);
        report.check(name+"_explicit_clear_and_regular_have_distinct_filtering",centerContrast(frame,184)-centerContrast(frame,64),12,true);
        auto sameMaterial=state;sameMaterial.controls[0].shape[2]=0;
        const auto equal=current.material(source,sameMaterial).output;
        float difference=0;
        for(unsigned y=44;y<116;++y)for(unsigned x=36;x<92;++x)
            difference=std::max(difference,delta(equal[y*Width+x],equal[y*Width+x+120],4));
        report.check(name+"_same_material_has_equal_flat_filtering",difference,0);
    }
}
