// Execute the production renderer's GPU extraction/filter methods directly.
// Never initialize it: no HWND, helper, WGC session or desktop input is created.
// Match the production TU's existing member-shadowing policy; keep /W4 /WX
// for this harness. Including it first also keeps test Width/Height names out
// of the Windows SDK's DirectXMath header scope.
#pragma warning(push)
#pragma warning(disable:4458)
#include "../../src-tauri/native/glass_renderer.cpp"
#pragma warning(pop)
#define wmain unused_isolated_regression_entrypoint
#include "glass_gpu_regression.cpp"
#undef wmain

namespace {
struct StateEvent { int32_t state, error; uint64_t geometry; };
void record_state(void* context, int32_t state, int32_t error, uint64_t geometry) {
    static_cast<std::vector<StateEvent>*>(context)->push_back({state,error,geometry});
}

Pixels read_level_zero(Gpu& gpu, ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    desc.MipLevels=desc.ArraySize=1;desc.MiscFlags=desc.BindFlags=0;
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    require(gpu.device->CreateTexture2D(&desc,nullptr,&staging),"level-zero staging texture");
    gpu.context->CopySubresourceRegion(staging.Get(),0,0,0,0,source,0,nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};require(gpu.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"reserve map");
    Pixels result;result.reserve(size_t(desc.Width)*desc.Height);
    for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x){
        const auto* p=static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*4;
        result.push_back({p[2]/255.f,p[1]/255.f,p[0]/255.f,p[3]/255.f});
    }
    gpu.context->Unmap(staging.Get(),0);return result;
}
void attach_gpu(TiezGlass& renderer,Gpu& gpu) {
    renderer.device.copy_from(gpu.device.Get());renderer.context.copy_from(gpu.context.Get());
    renderer.vertex.copy_from(gpu.vertex.Get());renderer.blur.copy_from(gpu.blur.Get());
    renderer.constants.copy_from(gpu.constants.Get());renderer.sampler.copy_from(gpu.sampler.Get());
}
Pixels crop_oracle(const Pixels& image,const RECT& source,const RECT& viewport) {
    Pixels result;
    for(int y=viewport.top;y<viewport.bottom;++y)for(int x=viewport.left;x<viewport.right;++x)
        result.push_back(image[size_t(y-source.top)*Width+x-source.left]);
    return result;
}
}

int wmain(int argc,wchar_t** argv) {
    try{
        if(argc!=3)throw std::runtime_error("Expected production shader and report");
        Gpu gpu(argv[1]);Report report;
        {
            static_assert(offsetof(TiezGlassState,geometry_revision)==48);
            static_assert(sizeof(TiezGlassState)==56);
            std::vector<StateEvent> events;
            TiezGlass renderer(nullptr,nullptr,&events,record_state);
            renderer.report(1,S_OK,41);
            const auto oldParameters=renderer.parameterRevision;
            renderer.synchronizing=true;
            TiezGlassState update{};
            update.viewport_width=425;update.viewport_height=589;update.blur_radius=1.5f;
            update.geometry_revision=42;
            const auto result=tiez_glass_update(&renderer,&update);
            report.check("deferred_update_returns_success_without_a_new_submission",result==S_OK&&
                renderer.parameters.geometry_revision==42&&renderer.frameDeferred&&events.size()==1?0:1,0);
            report.check("deferred_update_keeps_the_previous_frame_receipt",renderer.reportedGeometryRevision==41?0:1,0);
            renderer.report_frame_submission(oldParameters,false);
            report.check("reentrant_parameter_change_cannot_acknowledge_an_old_frame",events.size()==1&&renderer.frameDeferred?0:1,0);
            renderer.report_frame_submission(renderer.parameterRevision,false);
            report.check("ready_to_ready_reports_the_new_geometry",events.size()==2&&events.back().geometry==42?0:1,0);
            renderer.report(1,S_OK,42);
            report.check("repeated_submission_does_not_spam_status",events.size()==2?0:1,0);
            renderer.report(3,S_OK,43);
            report.check("background_only_output_cannot_acknowledge_controls",events.back().state==3&&events.back().geometry==0?0:1,0);
            renderer.report(0,S_OK,43);
            report.check("withdrawal_clears_geometry_acknowledgement",events.back().state==0&&events.back().geometry==0?0:1,0);
            renderer.failed=true;renderer.report(2,E_FAIL,43);
            const auto failedCount=events.size();
            renderer.report(1,S_OK,44);
            report.check("failed_renderer_cannot_publish_a_geometry_ack",events.size()==failedCount&&events.back().geometry==0?0:1,0);
            renderer.failed=false;renderer.tearingDown=true;renderer.report(1,S_OK,45);
            report.check("teardown_does_not_emit_late_geometry",events.size()==failedCount?0:1,0);
            MSG message{};
            while(PeekMessageW(&message,reinterpret_cast<HWND>(INT_PTR(-1)),reconcileMessage,reconcileMessage,PM_REMOVE)){}
        }
        const RECT reserve{-640,-320,-640+int(Width),-320+int(Height)};
        Pixels pixels;
        for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
            const float alpha=(x<11||y<7)?0.f:(x/13+y/9)%3==0?.5f:1.f;
            pixels.push_back({alpha*float(x%29)/28,alpha*float(y%23)/22,alpha*((x/17+y/11)%2?.9f:.1f),alpha});
        }
        auto raw=gpu.texture(&pixels);
        const auto quantized=read_level_zero(gpu,raw.texture.Get());
        {
            // PostMessage(nullptr) targets this test thread's own queue. This
            // exercises the production watchdog and explicit recovery request
            // without creating a window, capture session or desktop input.
            MSG message{};
            const HWND threadMessages=reinterpret_cast<HWND>(INT_PTR(-1));
            PeekMessageW(&message,threadMessages,reconcileMessage,reconcileMessage,PM_NOREMOVE);
            TiezGlass renderer(nullptr,nullptr,nullptr,nullptr);
            const auto dispatch=std::make_shared<Dispatch>();
            renderer.dispatch=dispatch;renderer.capturedScene.copy_from(raw.texture.Get());
            renderer.validTexture=renderer.resizeBackground=renderer.viewportResizePending=true;
            renderer.frameDeferred=true;renderer.resizeSizeGate.begin();
            renderer.resizeLease={GetTickCount64(),true};
            renderer.expire_resize_background();
            report.check("live_resize_lease_does_not_restart_the_capture",renderer.dispatch==dispatch&&
                renderer.capturedScene&&renderer.resizeSizeGate.waiting?0:1,0);
            renderer.resizeLease={0,true};
            renderer.synchronizing=true;renderer.expire_resize_background();
            report.check("watchdog_does_not_reenter_an_active_output_transaction",renderer.dispatch==dispatch?0:1,0);
            renderer.synchronizing=false;renderer.expire_resize_background();
            report.check("expired_resize_retires_the_previous_dispatch",!dispatch->alive.load()&&!renderer.dispatch?0:1,0);
            report.check("expired_resize_clears_pixels_and_size_gate",!renderer.capturedScene&&!renderer.validTexture&&
                !renderer.resizeBackground&&!renderer.viewportResizePending&&!renderer.resizeSizeGate.waiting?0:1,0);
            const bool queued=PeekMessageW(&message,threadMessages,reconcileMessage,reconcileMessage,PM_REMOVE)!=0;
            report.check("cancelled_resize_expiry_queues_recovery_without_waiting_for_a_new_frame",
                queued&&message.wParam==renderer.identity?0:1,0);
        }
        for(const RECT viewport:std::array<RECT,4>{{{-624,-304,-504,-224},{-576,-288,-448,-192},
            {-625,-304,-488,-207},{-640,-320,-496,-216}}}) {
            TiezGlass renderer(nullptr,nullptr,nullptr,nullptr);attach_gpu(renderer,gpu);
            renderer.bounds=viewport;renderer.width=UINT(viewport.right-viewport.left);renderer.height=UINT(viewport.bottom-viewport.top);
            renderer.lastCapture.crop=TiezGlass::pixel_rect(reserve);renderer.lastCapture.captured_at=12345;
            renderer.capturedScene.copy_from(raw.texture.Get());
            renderer.make_texture(renderer.scene,renderer.sceneView,nullptr,true);
            renderer.make_texture(renderer.tinted,renderer.tintedView,&renderer.tintedTarget);
            if(!renderer.copy_captured_viewport())throw std::runtime_error("Production viewport extraction rejected a known source");
            const std::string label="viewport"+std::to_string(viewport.left)+"_"+std::to_string(viewport.top);
            report.check(label+"_raw_copy_has_exact_world_coordinates",
                maxDelta(read_level_zero(gpu,renderer.scene.get()),crop_oracle(quantized,reserve,viewport),4),0);
            ShaderState lastSettings{};
            for(float dpi:{1.f,1.5f,2.f})for(float sigma:{1.5f,8.75f,16.f})for(bool dark:{false,true}){
                State reference;reference.options[0]=sigma*dpi;reference.options[3]=dpi;
                const float light[]{245.f/255,248.f/255,252.f/255},night[]{23.f/255,31.f/255,43.f/255};
                std::copy_n(dark?night:light,3,reference.tint);reference.tint[3]=.74f;
                const auto expected=gpu.renderBlur(pixels,reference);
                ShaderState settings{};static_assert(sizeof(settings)==sizeof(reference));
                std::memcpy(&settings,&reference,sizeof(settings));
                settings.size[0]=float(renderer.width);settings.size[1]=float(renderer.height);
                lastSettings=settings;
                if(!renderer.render_capture_background(settings))throw std::runtime_error("Production reserve filter failed");
                const auto actual=read_level_zero(gpu,renderer.tinted.get());
                const std::string name=label+"_dpi"+std::to_string(dpi)+"_sigma"+std::to_string(sigma)+(dark?"_dark":"_light");
                report.check(name+"_viewport_is_crop_of_complete_filtered_capture",
                    maxDelta(actual,crop_oracle(expected,reserve,viewport),4),1.01/255);
                report.check(name+"_full_background_surface_remains_available",
                    maxDelta(read_level_zero(gpu,renderer.captureFiltered.texture.get()),expected,4),1.01/255);
                report.check(name+"_viewport_is_opaque",alphaError(actual),0);
                report.check(name+"_metadata_remains_screen_anchored",
                    renderer.filteredCaptureValid&&EqualRect(&renderer.filteredCaptureBounds,&reserve)&&
                    renderer.lastCapture.captured_at==12345?0:1,0);
            }
            // A cached crop must do zero pixel-shader work. A real new source
            // revision must filter again even when its texture object is reused.
            auto invocations=[&]{
                D3D11_QUERY_DESC description{D3D11_QUERY_PIPELINE_STATISTICS,0};
                ComPtr<ID3D11Query> query;require(gpu.device->CreateQuery(&description,&query),"filter statistics");
                gpu.context->Begin(query.Get());
                if(!renderer.render_capture_background(lastSettings))throw std::runtime_error("cached reserve");
                gpu.context->End(query.Get());D3D11_QUERY_DATA_PIPELINE_STATISTICS statistics{};
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
                while(gpu.context->GetData(query.Get(),&statistics,sizeof(statistics),0)==S_FALSE){
                    if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("filter statistics timeout");Sleep(1);
                }
                return statistics.PSInvocations;
            };
            report.check(label+"_unchanged_background_does_no_filter_draws",double(invocations()),0);
            const RECT moved{viewport.left+1,viewport.top,viewport.right+1,viewport.bottom};
            if(tiez_glass_detail::contains_rect(TiezGlass::pixel_rect(reserve),TiezGlass::pixel_rect(moved))){
                const auto filtered=read_level_zero(gpu,renderer.captureFiltered.texture.get());
                renderer.bounds=moved;
                report.check(label+"_move_inside_reserve_does_no_filter_draws",double(invocations()),0);
                report.check(label+"_cached_move_keeps_exact_background_coordinates",maxDelta(read_level_zero(gpu,renderer.tinted.get()),crop_oracle(filtered,reserve,moved),4),0);
                renderer.bounds=viewport;
            }
            ++renderer.captureRevision;
            report.check(label+"_new_frame_in_the_same_texture_invalidates_filter",double(invocations()),1,true);
            const auto existing=read_level_zero(gpu,renderer.tinted.get());
            renderer.bounds.left=reserve.left-1;
            report.check(label+"_uncaptured_region_cannot_be_claimed_as_raw",renderer.copy_captured_viewport()?1:0,0);
            report.check(label+"_uncaptured_region_cannot_be_claimed_as_filtered",renderer.render_capture_background({})?1:0,0);
            report.check(label+"_failed_reserve_does_not_overwrite_the_viewport",maxDelta(existing,read_level_zero(gpu,renderer.tinted.get()),4),0);
            renderer.bounds=viewport;renderer.lastCapture.crop.right-=1;
            bool rejected=false;
            try{renderer.copy_captured_viewport();}catch(const winrt::hresult_error&){rejected=true;}
            report.check(label+"_source_metadata_must_match_its_actual_texture",rejected?0:1,0);
        }
        // Normalized convolution of one constant colour must retain that
        // colour in the low-coverage tail too. White-only fixtures hide loss
        // of relative RGB/alpha precision when all four channels are equal.
        for(unsigned alpha:{64u,255u}) {
            const Pixel covered{float(alpha*5/16)/255,float(alpha*19/32)/255,
                float(alpha*7/8)/255,float(alpha)/255};
            Pixels halfPlane(Width*Height,Pixel{0,0,0,0});
            for(unsigned y=0;y<Height;++y)for(unsigned x=Width/2;x<Width;++x)
                halfPlane[y*Width+x]=covered;
            auto source=gpu.texture(&halfPlane);
            TiezGlass renderer(nullptr,nullptr,nullptr,nullptr);attach_gpu(renderer,gpu);
            renderer.bounds=reserve;renderer.width=Width;renderer.height=Height;
            renderer.lastCapture.crop=TiezGlass::pixel_rect(reserve);
            renderer.capturedScene.copy_from(source.texture.Get());
            renderer.make_texture(renderer.tinted,renderer.tintedView,&renderer.tintedTarget);
            for(float sigma:{1.5f,16.f,32.f})for(bool dark:{false,true})for(float tint:{0.f,.74f,.90f}) {
                ShaderState settings{};settings.size[0]=Width;settings.size[1]=Height;
                settings.options[0]=sigma;settings.options[3]=1;
                const float light[]{245.f/255,248.f/255,252.f/255},night[]{23.f/255,31.f/255,43.f/255};
                std::copy_n(dark?night:light,3,settings.tint);settings.tint[3]=tint;
                if(!renderer.render_capture_background(settings))throw std::runtime_error("Coverage-tail filter failed");
                auto actual=read_level_zero(gpu,renderer.tinted.get());
                Pixel expected{0,0,0,1};
                for(unsigned c=0;c<3;++c)expected[c]=covered[c]/covered[3]*(1-tint)+settings.tint[c]*tint;
                float error=0;
                const auto first=unsigned(std::ceil(Width/2.f-2.5f*sigma));
                for(unsigned x=first;x<Width;++x)
                    error=std::max(error,delta(actual[(Height/2)*Width+x],expected));
                const auto name="constant_colour_tail_alpha"+std::to_string(alpha)+"_sigma"+
                    std::to_string(sigma)+(dark?"_dark":"_light")+"_tint"+std::to_string(tint);
                report.check(name,error,1.01/255);
            }
        }
        return report.write(argv[2],gpu.level)?0:1;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
}
