// Production GPU methods and hidden geometry fixtures; no desktop capture.
#pragma warning(push)
#pragma warning(disable:4458)
#include "../../src-tauri/native/glass_renderer.cpp"
#pragma warning(pop)
#define TIEZ_GPU_WIDTH 640
#define TIEZ_GPU_HEIGHT 896
#define wmain unused_gpu_regression_entrypoint
#include "glass_gpu_regression.cpp"
#undef wmain
#include <functional>
#include "glass_present_stub.h"

namespace {
struct StateEvent { int32_t state; uint64_t geometry; };
void record(void* user,int32_t state,int32_t,uint64_t geometry) {
    static_cast<std::vector<StateEvent>*>(user)->push_back({state,geometry});
}
struct HiddenWindow {
    HWND value = nullptr;
    HiddenWindow() {
        WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"TieZ.Glass.OffscreenRegression";
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("test window class");
        value=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"",WS_POPUP,-30000,-30000,640,896,nullptr,nullptr,cls.hInstance,nullptr);
        if(!value)throw std::runtime_error("hidden test window");
    }
    ~HiddenWindow(){if(value)DestroyWindow(value);}
};
void submission_checks(Report& report) {
    HiddenWindow host;PresentStub swap;std::vector<StateEvent> events;
    TiezGlass r(host.value,nullptr,&events,record);r.swap.copy_from(&swap);
    r.nativeStarted=r.foregroundSwapMounted=r.validTexture=true;r.parameterRevision=7;r.parameters.geometry_revision=43;
    r.report(1,S_OK,42);const auto epoch=r.presentationEpoch.value;
    r.pendingPresent={epoch,7,true,false,false,true};
    for(const HRESULT result:{DXGI_ERROR_WAS_STILL_DRAWING,DXGI_STATUS_OCCLUDED,DXGI_ERROR_WAS_STILL_DRAWING}){
        swap.result=result;
        report.check("backpressure_retains_pending_buffer",r.submit_pending_present()?1:0,0);
        report.check("backpressure_never_withdraws_mounted_image",r.nativeStarted&&r.foregroundSwapMounted&&r.validTexture&&!r.failed?0:1,0);
        report.check("backpressure_cannot_acknowledge_unsubmitted_geometry",events.size()==1&&r.reportedGeometryRevision==42?0:1,0);
    }
    swap.result=S_OK;
    report.check("retry_submits_without_redrawing",r.submit_pending_present()&&!r.pendingPresent.active&&swap.calls==4?0:1,0);
    report.check("only_success_confirms_new_geometry",events.size()==2&&events.back().geometry==43?0:1,0);
    report.check("present_is_nonblocking",swap.interval==0&&swap.flags==DXGI_PRESENT_DO_NOT_WAIT?0:1,0);
    auto signal=std::make_shared<Dispatch>();signal->window=host.value;signal->generation=17;r.dispatch=signal;r.baseDirty=false;
    r.pendingPresent={epoch,7,true,false,false,true};r.parameterRevision=8;
    {TiezGlass::SynchronizationScope scope(r);r.submit_pending_present();}
    report.check("successful_retry_schedules_newer_controls_on_static_background",signal->frameQueued.load()?0:1,0);
    signal->frameQueued.store(false);r.pendingPresent={epoch,8,true,false,false,true};r.baseDirty=true;
    {TiezGlass::SynchronizationScope scope(r);r.submit_pending_present();}
    report.check("successful_retry_schedules_newer_background_pixels",signal->frameQueued.load()?0:1,0);
    signal->frameQueued.store(false);r.pendingPresent={epoch,8,true,false,false,true};r.baseDirty=false;
    {TiezGlass::SynchronizationScope scope(r);r.submit_pending_present();}
    report.check("successful_retry_does_not_create_an_idle_render_loop",!signal->frameQueued.load()?0:1,0);
    r.dispatch.reset();
    r.pendingPresent={epoch,7,true,false,false,true};swap.result=DXGI_ERROR_DEVICE_REMOVED;
    bool removed=false;try{r.submit_pending_present();}catch(const winrt::hresult_error& error){removed=error.code()==DXGI_ERROR_DEVICE_REMOVED;}
    report.check("device_loss_is_not_treated_as_temporary_backpressure",removed?0:1,0);
    r.pendingPresent={};r.awaitingMoveCapture=true;
    const auto before=swap.calls;r.render();
    report.check("move_wait_does_not_resample_previous_origin",swap.calls==before&&r.reportedGeometryRevision==43?0:1,0);
    report.check("test_window_is_hidden_and_receives_no_input",!IsWindowVisible(host.value)?0:1,0);
}
void retarget_checks(Report& report) {
    TiezGlass r(nullptr,nullptr,nullptr,nullptr);
    auto signal=std::make_shared<Dispatch>();r.dispatch=signal;
    r.cropRevision=6;r.retargetFlushPending=true;
    signal->requestedRetarget=6;signal->retargetWorkerRunning=true;
    report.check("retarget_does_not_consume_frames_before_compositor_fence",!r.retarget_ready()?0:1,0);
    report.check("retarget_coalesces_newer_crop_on_same_worker",complete_retarget_flush(signal,5,E_FAIL)&&signal->retargetWorkerRunning?0:1,0);
    report.check("old_crop_completion_cannot_release_new_crop",!r.retarget_ready()&&r.retargetFlushPending?0:1,0);
    report.check("latest_fence_stops_worker",!complete_retarget_flush(signal,6,S_OK)&&!signal->retargetWorkerRunning?0:1,0);
    report.check("latest_fence_releases_static_scene_without_another_capture",r.retarget_ready()&&!r.retargetFlushPending?0:1,0);
    complete_retarget_flush(signal,5,E_FAIL);
    report.check("out_of_order_completion_cannot_overwrite_new_fence",signal->completedRetarget==6&&signal->retargetResult==S_OK?0:1,0);
    r.cropRevision=7;r.retargetFlushPending=true;signal->requestedRetarget=7;
    complete_retarget_flush(signal,7,E_FAIL);
    bool failed=false;try{r.retarget_ready();}catch(const winrt::hresult_error& error){failed=error.code()==E_FAIL;}
    report.check("compositor_failure_never_accepts_unverified_crop",failed&&r.retargetFlushPending?0:1,0);
    signal->alive.store(false);signal->retargetWorkerRunning=true;
    complete_retarget_flush(signal,8,S_OK);queue(signal,true);
    report.check("closed_renderer_discards_late_worker_completion",signal->completedRetarget==7&&!signal->retargetWorkerRunning&&!signal->frameQueued.load()?0:1,0);
    r.dispatch.reset();
}
void frame_bounds_checks(Report& report) {
    HiddenWindow host;
    TiezGlass r(host.value,nullptr,nullptr,nullptr);r.save_region();
    for(const UINT dpi:{96u,120u,144u,168u,192u}){
        const int w=int(425u*dpi/96),h=int(589u*dpi/96);
        if(!SetWindowPos(host.value,nullptr,-30000,-30000,w,h,SWP_NOACTIVATE|SWP_NOZORDER))throw std::runtime_error("hidden resize");
        RECT outer{};GetWindowRect(host.value,&outer);const RECT client=r.client_bounds();
        r.bounds=client;r.hostBounds=outer;r.width=UINT(w);r.height=UINT(h);
        r.install_host_region(client,outer,dpi);
        Region mask(r.client_region());RECT extent{};GetRgnBox(mask.value,&extent);
        report.check("dpi"+std::to_string(dpi)+"_glass_has_no_nonclient_gutter",EqualRect(&client,&outer)?0:1,0);
        report.check("dpi"+std::to_string(dpi)+"_rounded_mask_covers_every_body_edge",
            extent.left==0&&extent.top==0&&extent.right==w&&extent.bottom==h&&
            PtInRegion(mask.value,0,h/2)&&PtInRegion(mask.value,w-1,h/2)&&PtInRegion(mask.value,w/2,0)&&PtInRegion(mask.value,w/2,h-1)?0:1,0);
        report.check("dpi"+std::to_string(dpi)+"_corner_is_clipped_without_a_rectangular_frame",!PtInRegion(mask.value,0,0)?0:1,0);
    }
}
void attach(TiezGlass& r, Gpu& gpu) {
    r.device.copy_from(gpu.device.Get()); r.context.copy_from(gpu.context.Get());
    r.vertex.copy_from(gpu.vertex.Get()); r.blur.copy_from(gpu.blur.Get());
    r.constants.copy_from(gpu.constants.Get()); r.sampler.copy_from(gpu.sampler.Get());
    require(r.device->CreatePixelShader(g_glass_renderer_prefix_init_ps, sizeof(g_glass_renderer_prefix_init_ps), nullptr, r.prefixInit.put()), "prefix init");
    require(r.device->CreatePixelShader(g_glass_renderer_prefix_glass_ps, sizeof(g_glass_renderer_prefix_glass_ps), nullptr, r.prefixGlass.put()), "prefix glass");
    require(r.device->CreatePixelShader(g_glass_renderer_prefix_track_ps, sizeof(g_glass_renderer_prefix_track_ps), nullptr, r.prefixTrack.put()), "prefix track");
    require(r.device->CreatePixelShader(g_glass_renderer_prefix_finish_ps, sizeof(g_glass_renderer_prefix_finish_ps), nullptr, r.prefixFinish.put()), "prefix finish");
    r.width=Width; r.height=Height;
    r.make_texture(r.scene,r.sceneView,nullptr,true);
    r.make_texture(r.tinted,r.tintedView,&r.tintedTarget);
    r.make_texture(r.initialPrefix.texture,r.initialPrefix.view,&r.initialPrefix.target,true,DXGI_FORMAT_R16G16B16A16_FLOAT);
    for(auto& s:r.prefixColors)r.make_texture(s.texture,s.view,&s.target,true,DXGI_FORMAT_R16G16B16A16_FLOAT);
    for(auto& s:r.prefixCoverage)r.make_texture(s.texture,s.view,&s.target,false,DXGI_FORMAT_R16_FLOAT);
}
void startup_alpha_checks(Gpu& gpu,Report& report) {
    HiddenWindow helper;TiezGlass r(nullptr,nullptr,nullptr,nullptr);
    r.device.copy_from(gpu.device.Get());r.context.copy_from(gpu.context.Get());r.helper=helper.value;
    D3D11_TEXTURE2D_DESC description{};description.Width=5;description.Height=1;description.MipLevels=description.ArraySize=description.SampleDesc.Count=1;
    description.Format=DXGI_FORMAT_B8G8R8A8_UNORM;description.Usage=D3D11_USAGE_STAGING;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    require(gpu.device->CreateTexture2D(&description,nullptr,r.initialAlpha.put()),"startup staging");
    for(unsigned alpha:{0u,64u,255u}){
        description.Usage=D3D11_USAGE_DEFAULT;description.CPUAccessFlags=0;
        const std::array<uint32_t,5> black{{alpha<<24,alpha<<24,alpha<<24,alpha<<24,alpha<<24}};
        D3D11_SUBRESOURCE_DATA data{black.data(),20,20};ComPtr<ID3D11Texture2D> texture;
        require(gpu.device->CreateTexture2D(&description,&data,&texture),"startup source");
        gpu.context->CopyResource(r.initialAlpha.get(),texture.Get());r.initialAlphaPending=true;
        bool accepted=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(r.initialAlphaPending){accepted=r.initial_alpha_ready();if(r.initialAlphaPending)Sleep(1);if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("nonblocking alpha timeout");}
        report.check("startup_alpha"+std::to_string(alpha)+"_never_accepts_empty_pixels",accepted==(alpha==255)?0:1,0);
    }
}
// Frozen pre-fix full-window algorithm is the optical oracle.
void legacy(TiezGlass& r, ID3D11RenderTargetView* output, const ShaderState& settings) {
    r.bind_pass_pipeline();
    r.draw_prefix_node(r.sceneView.get(),nullptr,r.initialPrefix,r.prefixCoverage[0],r.prefixInit.get(),settings);
    ID3D11ShaderResourceView* current=r.initialPrefix.view.get(); unsigned index=0;
    const auto node=[&](ID3D11PixelShader* shader,const ShaderState& state){
        const unsigned next=1-index;
        r.draw_prefix_node(current,r.prefixCoverage[index].view.get(),r.prefixColors[next],r.prefixCoverage[next],shader,state);
        index=next;current=r.prefixColors[next].view.get();
    };
    for(size_t i=0;i<r.controls.size();++i){
        ShaderState state=settings;state.controls[0]=settings.controls[i];
        for(size_t t=0;t<size_t(settings.trackMeta[0]);++t)if(size_t(settings.tracks[t].shape[2])==i){state.tracks[0]=settings.tracks[t];node(r.prefixTrack.get(),state);}
        node(r.prefixGlass.get(),state);
    }
    ID3D11ShaderResourceView* sources[]{r.initialPrefix.view.get(),r.prefixCoverage[index].view.get(),r.tintedView.get()};
    r.context->PSSetShaderResources(1,3,sources);
    r.draw_pass(current,output,r.prefixFinish.get(),settings,r.initialPrefix.view.get());
    ID3D11ShaderResourceView* none[3]{};r.context->PSSetShaderResources(1,3,none);
}
struct Timing {double gpuMs, cpuMs;};
Timing timed(Gpu& gpu,const std::function<void()>& draw) {
    D3D11_QUERY_DESC description{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
    ComPtr<ID3D11Query> span,begin,end;
    require(gpu.device->CreateQuery(&description,&span),"timing span");
    description.Query=D3D11_QUERY_TIMESTAMP;
    require(gpu.device->CreateQuery(&description,&begin),"timing begin");require(gpu.device->CreateQuery(&description,&end),"timing end");
    gpu.context->Begin(span.Get());gpu.context->End(begin.Get());
    const auto started=std::chrono::steady_clock::now();draw();
    const auto submitted=std::chrono::steady_clock::now();
    gpu.context->End(end.Get());gpu.context->End(span.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};UINT64 first=0,last=0;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(gpu.context->GetData(span.Get(),&disjoint,sizeof(disjoint),0)==S_FALSE){
        if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("GPU timestamp timeout");Sleep(1);
    }
    require(gpu.context->GetData(begin.Get(),&first,sizeof(first),0),"begin timestamp");
    require(gpu.context->GetData(end.Get(),&last,sizeof(last),0),"end timestamp");
    if(disjoint.Disjoint||!disjoint.Frequency)throw std::runtime_error("Disjoint GPU sample");
    return {1000.*double(last-first)/double(disjoint.Frequency),std::chrono::duration<double,std::milli>(submitted-started).count()};
}
Pixels pattern(unsigned frame) {
    Pixels result;result.reserve(size_t(Width)*Height);
    for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
        const float alpha=x<4||y<4?0.f:((x/23+y/17)%7==0?.5f:1.f);
        const float ink=(x+frame*3)%43<3||(y+frame)%31<2?.08f:.94f;
        result.push_back({alpha*ink,alpha*(.2f+.7f*float((x/19+frame)%5)/4),alpha*(.1f+.8f*float((y/29+frame)%7)/6),alpha});
    }return result;
}
void source(TiezGlass& r,Gpu& gpu,unsigned frame) {
    const auto pixels=pattern(frame);auto raw=gpu.texture(&pixels);
    r.context->CopySubresourceRegion(r.scene.get(),0,0,0,0,raw.texture.Get(),0,nullptr);
    r.context->GenerateMips(r.sceneView.get());r.context->CopyResource(r.tinted.get(),raw.texture.Get());
}
ShaderState layout(TiezGlass& r,unsigned count,unsigned frame) {
    ShaderState state{};state.size[0]=float(Width);state.size[1]=float(Height);state.options[0]=8.75f;state.options[1]=float(count);state.options[2]=1;state.options[3]=1;
    state.tint[0]=.96f;state.tint[1]=.97f;state.tint[2]=.99f;state.tint[3]=.78f;
    r.controls.resize(count);
    for(unsigned i=0;i<count;++i){
        auto& c=state.controls[i];c.rectangle[0]=12.f+156.f*(i%4)+float(frame%3);c.rectangle[1]=12.f+54.f*(i/4);c.rectangle[2]=140;c.rectangle[3]=42;
        c.shape[0]=21;c.shape[1]=float(frame%5)/4;c.shape[2]=float(i%3);
    }
    if(count>8){ // Overlapping reading plane, rounded controls and slider tracks.
        auto& c=state.controls[0];c.rectangle[0]=7;c.rectangle[1]=7;c.rectangle[2]=626;c.rectangle[3]=880;c.shape[0]=24;c.shape[2]=2;
        for(unsigned i=2;i<count;i+=13){auto& t=state.tracks[size_t(state.trackMeta[0])];
            std::copy_n(state.controls[i].rectangle,4,t.rectangle);t.rectangle[1]+=18;t.rectangle[3]=8;
            t.shape[0]=4;t.shape[1]=float(frame%11)/10;t.shape[2]=float(i);t.active[0]=.1f;t.active[1]=.4f;t.active[2]=.9f;t.active[3]=1;
            t.inactive[0]=.1f;t.inactive[1]=.4f;t.inactive[2]=.9f;t.inactive[3]=.15f;state.trackMeta[0]+=1;
        }
    }return state;
}
void prepare_capture(TiezGlass& r) {
    constexpr UINT padding=169,w=Width+2*padding,h=Height+2*padding;
    r.bounds={LONG(padding),LONG(padding),LONG(padding+Width),LONG(padding+Height)};
    r.captureBounds={0,0,LONG(w),LONG(h)};r.lastCapture.crop=TiezGlass::pixel_rect(r.captureBounds);
    r.make_texture(r.captureTexture,r.captureView,nullptr,true,DXGI_FORMAT_B8G8R8A8_UNORM,w,h);
    std::vector<uint32_t> pixels(size_t(w)*h);
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){
        const unsigned ink=(x%43<3||y%31<2)?20u:240u;
        pixels[size_t(y)*w+x]=0xff000000u|(ink<<16)|((40u+(x/19)%5*40u)<<8)|(30u+(y/29)%7*30u);
    }
    r.context->UpdateSubresource(r.captureTexture.get(),0,nullptr,pixels.data(),w*4,0);
    r.capturedScene=r.captureTexture;++r.captureRevision;
    if(!r.copy_captured_viewport())throw std::runtime_error("pipeline viewport");
}
void analytic_background(TiezGlass& r,const ShaderState& settings) {
    ID3D11Buffer* noKernel=nullptr;r.context->PSSetConstantBuffers(1,1,&noKernel);
    D3D11_TEXTURE2D_DESC d{};r.capturedScene->GetDesc(&d);
    ShaderState full=settings;full.size[0]=float(d.Width);full.size[1]=float(d.Height);full.options[1]=full.trackMeta[0]=0;
    full.direction[0]=1;full.direction[1]=0;full.options[2]=0;r.bind_pass_pipeline(d.Width,d.Height);
    r.draw_pass(r.filteredCaptureSourceView.get(),r.captureHorizontal.target.get(),r.blur.get(),full);
    full.direction[0]=0;full.direction[1]=1;full.options[2]=1;
    r.draw_pass(r.captureHorizontal.view.get(),r.captureFiltered.target.get(),r.blur.get(),full);
    const D3D11_BOX crop{169,169,0,169+Width,169+Height,1};
    r.context->CopySubresourceRegion(r.tinted.get(),0,0,0,0,r.captureFiltered.texture.get(),0,&crop);
    r.bind_pass_pipeline();
}
void render_fixture(Gpu& gpu,const wchar_t* geometry,const wchar_t* output,bool dark) {
    std::ifstream file{std::filesystem::path(geometry)};
    unsigned w=0,h=0,count=0;file>>w>>h>>count;
    if(!file||w!=Width||h!=Height||count>64)throw std::runtime_error("fixture geometry header");
    TiezGlass r(nullptr,nullptr,nullptr,nullptr);attach(r,gpu);prepare_capture(r);r.controls.resize(count);
    ShaderState state{};state.size[0]=float(w);state.size[1]=float(h);
    state.options[0]=8.75f;state.options[1]=float(count);state.options[2]=state.options[3]=1;
    const float light[]{245.f/255,248.f/255,252.f/255},night[]{23.f/255,31.f/255,43.f/255};
    std::copy_n(dark?night:light,3,state.tint);state.tint[3]=.82f;
    for(unsigned i=0;i<count;++i){
        auto& c=state.controls[i];for(float& value:c.rectangle)file>>value;
        file>>c.shape[0]>>c.shape[1]>>c.shape[2];unsigned hasTrack=0;file>>hasTrack;
        if(hasTrack){auto& t=state.tracks[size_t(state.trackMeta[0]++)];for(float& value:t.rectangle)file>>value;
            file>>t.shape[0]>>t.shape[1];t.shape[2]=float(i);
            for(float& value:t.active)file>>value;for(float& value:t.inactive)file>>value;
        }
    }
    if(!file)throw std::runtime_error("fixture geometry body");
    if(!r.render_capture_background(state))throw std::runtime_error("fixture background");
    auto target=gpu.texture();r.render_prefix(target.target.Get(),state,true);const auto pixels=gpu.read(target);
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+w*h*4;
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(w);info.biHeight=-LONG(h);info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    std::ofstream image{std::filesystem::path(output),std::ios::binary};
    image.write(reinterpret_cast<const char*>(&header),sizeof(header));image.write(reinterpret_cast<const char*>(&info),sizeof(info));
    for(const auto& p:pixels){const uint8_t bgra[]{uint8_t(std::lround(p[2]*255)),uint8_t(std::lround(p[1]*255)),uint8_t(std::lround(p[0]*255)),255};image.write(reinterpret_cast<const char*>(bgra),4);}
    if(!image)throw std::runtime_error("fixture bitmap output");
}
}
#include "glass_runtime_regression.h"
int wmain(int argc,wchar_t** argv) {
    try{
        if(argc==4&&(std::wstring(argv[3])==L"--runtime"||std::wstring(argv[3])==L"--runtime-dark"))
            return runtime_regression(argv[2],std::wstring(argv[3])==L"--runtime-dark");
        if(argc!=3&&argc!=5)throw std::runtime_error("Expected shader and report, or shader/geometry/bitmap/mode");
        Gpu gpu(argv[1],D3D_DRIVER_TYPE_HARDWARE);
        if(argc==5){render_fixture(gpu,argv[2],argv[3],std::wstring(argv[4])==L"dark");return 0;}
        Report report;
        submission_checks(report);
        retarget_checks(report);
        frame_bounds_checks(report);
        startup_alpha_checks(gpu,report);
        TiezGlass r(nullptr,nullptr,nullptr,nullptr);attach(r,gpu);
        auto actual=gpu.texture(),expected=gpu.texture();
        std::ofstream file(argv[2]);file<<std::setprecision(6)<<"{\"width\":"<<Width<<",\"height\":"<<Height<<",\"device\":\"D3D11 hardware\",\"cases\":[";
        bool firstCase=true;
        for(unsigned count:{0u,8u,32u,64u}){
            double oldGpu=0,newGpu=0,oldCpu=0,newCpu=0,maxError=0;constexpr unsigned frames=12;
            for(unsigned f=0;f<frames;++f){
                source(r,gpu,f);const auto settings=layout(r,count,f);
                const auto old=timed(gpu,[&]{legacy(r,expected.target.Get(),settings);});
                const auto current=timed(gpu,[&]{r.bind_pass_pipeline();r.render_prefix(actual.target.Get(),settings,true);});
                oldGpu+=old.gpuMs;newGpu+=current.gpuMs;oldCpu+=old.cpuMs;newCpu+=current.cpuMs;
                const auto image=gpu.read(actual);maxError=std::max(maxError,double(maxDelta(image,gpu.read(expected),4)));
                report.check("frame"+std::to_string(f)+"_controls"+std::to_string(count)+"_ordered_optics",maxError,2.01/255);
                report.check("frame"+std::to_string(f)+"_controls"+std::to_string(count)+"_opaque_edge",alphaError(image),0);
            }
            // Timestamp a continuous burst as well. Single frames followed by
            // staging readback can include GPU scheduling/power-state latency.
            const auto steady=layout(r,count,0);source(r,gpu,0);
            constexpr unsigned burstFrames=32;
            const auto oldBurst=timed(gpu,[&]{for(unsigned i=0;i<burstFrames;++i)legacy(r,expected.target.Get(),steady);});
            const auto burst=timed(gpu,[&]{for(unsigned i=0;i<burstFrames;++i){r.bind_pass_pipeline();r.render_prefix(actual.target.Get(),steady,true);}});
            if(!firstCase)file<<',';firstCase=false;
            file<<"{\"controls\":"<<count<<",\"frames\":"<<frames<<",\"legacyGpuMs\":"<<oldGpu/frames<<",\"gpuMs\":"<<newGpu/frames<<",\"legacyCpuMs\":"<<oldCpu/frames<<",\"cpuMs\":"<<newCpu/frames<<",\"legacyBurstGpuMs\":"<<oldBurst.gpuMs/burstFrames<<",\"burstGpuMs\":"<<burst.gpuMs/burstFrames<<",\"burstCpuMs\":"<<burst.cpuMs/burstFrames<<",\"pixelError\":"<<maxError<<'}';
        }
        file<<"],\"pipeline\":[";firstCase=true;prepare_capture(r);
        for(unsigned count:{8u,64u})for(float sigma:{1.5f,8.75f,16.f}){
            auto settings=layout(r,count,0);settings.options[0]=sigma*1.5f;settings.options[3]=1.5f;
            if(!r.render_capture_background(settings))throw std::runtime_error("pipeline filter");
            constexpr unsigned n=16;
            const auto old=timed(gpu,[&]{for(unsigned f=0;f<n;++f){
                r.context->GenerateMips(r.captureView.get());r.copy_captured_viewport();analytic_background(r,settings);legacy(r,expected.target.Get(),settings);
            }});
            const auto current=timed(gpu,[&]{for(unsigned f=0;f<n;++f){
                ++r.captureRevision;r.context->GenerateMips(r.captureView.get());r.copy_captured_viewport();r.render_capture_background(settings);r.render_prefix(actual.target.Get(),settings,true);
            }});
            report.check("pipeline_sigma"+std::to_string(sigma)+"_controls"+std::to_string(count)+"_same_pixels",maxDelta(gpu.read(actual),gpu.read(expected),4),2.01/255);
            report.check("pipeline_sigma"+std::to_string(sigma)+"_controls"+std::to_string(count)+"_60Hz_budget",current.gpuMs/n,1000./60);
            if(!firstCase)file<<',';firstCase=false;
            file<<"{\"controls\":"<<count<<",\"dpi\":1.5,\"sigmaCss\":"<<sigma<<",\"legacyGpuMs\":"<<old.gpuMs/n<<",\"gpuMs\":"<<current.gpuMs/n<<",\"cpuMs\":"<<current.cpuMs/n<<'}';
        }
        const auto failures=std::count_if(report.results.begin(),report.results.end(),[](const Result& result){return !result.passed;});
        file<<"],\"checks\":"<<report.results.size()<<",\"failures\":"<<failures<<",\"scope\":\"Production GPU methods with synthetic dynamic backgrounds and control motion. Hidden Win32 geometry/timer fixtures and injected DXGI backpressure; no visible windows, WGC, desktop input or screen presentation.\"}";
        for(const auto& result:report.results)if(!result.passed)std::cerr<<result.name<<" metric="<<result.metric<<'\n';
        std::cout<<"GPU frame checks="<<report.results.size()<<" failures="<<failures<<'\n';return failures?1:0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
}
