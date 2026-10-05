#define TIEZ_PROJECTION_HELPER_ONLY
#include "glass_projection_regression.cpp"

State scene(Control control, float press = 0, float ior = 1.52f) {
    State s; s.controls[0] = control; s.controls[0].shape[1] = press;
    s.options[1] = 1; s.options[3] = 1; s.direction[0] = ior; s.direction[1] = 26;
    s.tint[0] = s.tint[1] = 0; s.tint[2] = s.tint[3] = 1; return s;
}
float coordError(const Pixels& a, const Pixels& b) {
    float result = 0;
    for (size_t i=0;i<a.size();++i) if(a[i][2]==1 && b[i][2]==1)
        result=std::max({result,std::abs(a[i][0]-b[i][0]),std::abs(a[i][1]-b[i][1]),std::abs(a[i][3]-b[i][3])});
    return result;
}
float continuity(const Pixels& points, const State& s) {
    float worst=0;
    for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x) {
        const auto i=y*Width+x;
        if(x+1<Width) worst=std::max(worst,std::hypot(points[i+1][0]-points[i][0]-s.tint[2],points[i+1][1]-points[i][1]));
        if(y+1<Height) worst=std::max(worst,std::hypot(points[i+Width][0]-points[i][0],points[i+Width][1]-points[i][1]-s.tint[3]));
    }
    return worst;
}
float flatCoreError(const Pixels& pixels,const State& state,float baseHeight,unsigned channel,bool pressDifference=false,const Pixels* unpressed=nullptr) {
    const auto& c=state.controls[0]; float r=c.shape[0];
    float baseIor=state.direction[0], dispersion=std::min(.16f,.28f*(baseIor-1));
    const double n=baseIor+(int(channel)-1)*dispersion;
    const double focal=std::max(6.0*baseHeight,.3*std::max(c.rect[2],c.rect[3]));
    const double p=c.shape[1], height=baseHeight*(1+.12*p*p*(3-2*p));
    float worst=0;
    for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x) {
        // Only the guaranteed flat region of these r16/r20 boxes. The oracle
        // uses a closed-form plane-slab equation, not HLSL refract/normalize.
        if(x+.5f<c.rect[0]+r+2 || x+.5f>c.rect[0]+c.rect[2]-r-2
            || y+.5f<c.rect[1]+r+2 || y+.5f>c.rect[1]+c.rect[3]-r-2) continue;
        auto i=y*Width+x;
        const double dx=(x+.5-c.rect[0]-.5*c.rect[2])/focal,dy=(y+.5-c.rect[1]-.5*c.rect[3])/focal;
        const double factor=n<=1?0:1/std::sqrt(n*n+(n*n-1)*(dx*dx+dy*dy))-1;
        const double ex=height*dx*factor,ey=height*dy*factor;
        if(pressDifference) {
            worst=std::max({worst,std::abs(pixels[i][0]-(x+.5f)-1.12f*((*unpressed)[i][0]-(x+.5f))),
                std::abs(pixels[i][1]-(y+.5f)-1.12f*((*unpressed)[i][1]-(y+.5f)))});
        } else worst=std::max({worst,float(std::abs(pixels[i][0]-(x+.5)-ex)),float(std::abs(pixels[i][1]-(y+.5)-ey))});
    }
    return worst;
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=8) throw std::runtime_error("Expected six shader paths and report path");
        ProjectionGpu current(argv[1]),fixed(argv[2]),moving(argv[3]),badAxis(argv[4]),flat(argv[5]),unbounded(argv[6]); Report report;
        constexpr float tolerance=1.0f/4096;
        const Control shapes[]={{{113,69,30,22},{11,0,0,0}},{{24,24,208,112},{16,0,0,0}},
            {{24,24,208,112},{20,0,0,0}},{{16,16,224,128},{64,0,0,0}},
            {{16,60,224,40},{11,0,0,0}},{{88,8,80,144},{8,0,0,0}}};
        constexpr unsigned ShapeCount=sizeof(shapes)/sizeof(shapes[0]);
        float worstDpi=0,worstDpiNegative=0,worstFold=1;
        unsigned count=0;
        for(unsigned shape=0;shape<ShapeCount;++shape) for(float press:{0.f,.5f,1.f})
        for(float phase:{0.f,.25f,.5f,.75f}) for(float ior:{1.f,1.52f,2.15f,2.5f}) for(unsigned channel=0;channel<3;++channel) {
            auto state=scene(shapes[shape],press,ior);
            state.controls[0].rect[0]+=phase; state.controls[0].rect[1]+=phase==0?0:1-phase;
            auto reference=current.coordinates(state,channel); auto metrics=inspect(reference,state);
            const auto name="field_"+std::to_string(count++);
            report.check(name+"_finite",metrics.nonfinite,0);
            report.check(name+"_inside_samples",metrics.opaque,10,true);
            report.check(name+"_pixel_triangles_preserve_orientation",metrics.minDet,.00001,true);
            report.check(name+"_source_stays_inside_surface",metrics.maxSourceOutside,tolerance);
            worstFold=std::min(worstFold,metrics.minDet);
            if(ior==1)report.check(name+"_unit_ior_identity",metrics.maxOffset,0);
            for(float dpi:{1.25f,1.5f,2.f,3.f}) {
                state.options[3]=dpi;
                auto scaled=current.coordinates(state,channel);
                const float error=coordError(reference,scaled);worstDpi=std::max(worstDpi,error);
                report.check(name+"_dpi"+std::to_string(dpi)+"_same_logical_field",error,tolerance);
                for(const auto& pixel:scaled) for(float value:pixel) if(!std::isfinite(value)) throw std::runtime_error("Nonfinite scaled probe");
            }
        }
        for(unsigned shape:{1u,2u})for(float press:{0.f,.5f,1.f})for(unsigned channel=0;channel<3;++channel) {
            auto state=scene(shapes[shape],press);auto pixels=current.coordinates(state,channel);
            report.check("closed_form_slab_"+std::to_string(shape)+"_"+std::to_string(press)+"_"+std::to_string(channel),
                flatCoreError(pixels,state,shape==1?12.f:15.f,channel),tolerance);
        }
        auto s=scene(shapes[1]); auto normal=current.coordinates(s,1),old=flat.coordinates(s,1);
        report.check("new_body_changes_flat_core",inspect(normal,s).maxPlatformOffset,1,true);
        report.check("old_flat_core_is_negative_control",inspect(old,s).maxPlatformOffset,0);
        auto fixedBase=fixed.coordinates(s,1);s.options[3]=2;
        worstDpiNegative=coordError(fixedBase,fixed.coordinates(s,1));
        report.check("fixed_pixel_camera_fails_dpi_gate",worstDpiNegative,.1,true);
        s=scene(shapes[1]);auto before=current.coordinates(s,1),movingBefore=moving.coordinates(s,1);s.controls[0].shape[1]=1;
        report.check("pressure_scales_flat_body_by_12_percent",flatCoreError(current.coordinates(s,1),s,12,1,true,&before),tolerance);
        report.check("moving_camera_erases_body_pressure_response",flatCoreError(moving.coordinates(s,1),s,12,1,true,&movingBefore),.1,true);
        float worstContinuity=0;
        for(unsigned shape=0;shape<ShapeCount;++shape)for(float press:{0.f,1.f})for(float ior:{1.52f,2.5f})for(unsigned channel=0;channel<3;++channel) {
            auto c=scene(shapes[shape],press,ior);
            c.tint[0]=c.controls[0].rect[0]+c.controls[0].shape[0]-1;
            c.tint[1]=c.controls[0].rect[1]+c.controls[0].shape[0]-1;
            c.tint[2]=c.tint[3]=1.f/64;
            const auto step=continuity(current.coordinates(c,channel),c);worstContinuity=std::max(worstContinuity,step);
            report.check("continuous_corner_axis_"+std::to_string(shape)+"_"+std::to_string(press)+"_"+std::to_string(ior)+"_"+std::to_string(channel),step,.05);
        }
        s=scene(shapes[1]);s.tint[0]=39;s.tint[1]=39;s.tint[2]=s.tint[3]=1.f/64;
        report.check("branch_normal_body_has_visible_axis_jump",continuity(badAxis.coordinates(s,1),s),.5,true);
        s=scene(shapes[2],1,2.5f);s.controls[0].rect[0]+=.25f;s.controls[0].rect[1]+=.75f;
        report.check("unbounded_projection_fails_orientation",inspect(unbounded.coordinates(s,2),s).minDet,0);
        for(unsigned shape=0;shape<ShapeCount;++shape)for(float press:{0.f,1.f})for(unsigned channel=0;channel<3;++channel) {
            const auto pixels=current.coordinates(scene(shapes[shape],press),channel);float error=0;
            for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x) {
                auto p=pixels[y*Width+x];if(p[2]!=1)continue;
                auto mx=pixels[y*Width+Width-1-x],my=pixels[(Height-1-y)*Width+x];
                error=std::max({error,std::abs(p[0]+mx[0]-Width),std::abs(p[1]-mx[1]),
                    std::abs(p[1]+my[1]-Height),std::abs(p[0]-my[0])});
            }
            report.check("four_corner_symmetry_"+std::to_string(shape)+"_"+std::to_string(press)+"_"+std::to_string(channel),error,tolerance);
        }
        for(unsigned channel=0;channel<3;++channel) {
            auto z=scene(shapes[1]); z.direction[1]=0;
            report.check("zero_thickness_identity_"+std::to_string(channel),inspect(current.coordinates(z,channel),z).maxOffset,0);
        }
        std::cout<<"diagnostic minimum orientation determinant="<<worstFold<<"; max DPI error="<<worstDpi
            <<"; fixed-camera error="<<worstDpiNegative<<"; max axis step="<<worstContinuity<<'\n';
        return report.write(argv[7],unsigned(current.level))?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
