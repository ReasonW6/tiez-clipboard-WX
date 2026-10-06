// Opt-in live compositor fixture. Both windows contain synthetic pixels only;
// there is no clipboard backend, WebView profile or input automation.
struct RuntimeEvents {
    std::vector<int32_t> states;
    HRESULT error = S_OK;
};
unsigned runtimePattern = 0;
double runtimeMaxDispatchMs = 0, runtimeMaxReadbackMs = 0, runtimeMaxSleepMs = 0;
void runtime_state(void* context, int32_t state, int32_t error, uint64_t) {
    auto& events = *static_cast<RuntimeEvents*>(context);
    events.states.push_back(state);
    if (state == 2) events.error = error;
}
struct RuntimeCapture {
    TiezGlass& renderer;
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    com_ptr<ID3D11Texture2D> staging;
    unsigned frames=0,blankReadyFrames=0,opaqueFailures=0;
    double lastRange=0;
    UINT width=0,height=0;
    std::vector<uint8_t> pixels;
    explicit RuntimeCapture(TiezGlass& value):renderer(value){
        const auto interop=get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForWindow(renderer.host,guid_of<GraphicsCaptureItem>(),put_abi(item)));
        pool=Direct3D11CaptureFramePool::CreateFreeThreaded(renderer.captureDevice,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,item.Size());
        session=pool.CreateCaptureSession(item);session.IsCursorCaptureEnabled(false);session.IsBorderRequired(false);session.StartCapture();
    }
    ~RuntimeCapture(){try{if(session)session.Close();if(pool)pool.Close();}catch(...){}}
    void drain(){
        Direct3D11CaptureFrame newest{nullptr};
        while(auto frame=pool.TryGetNextFrame()){if(newest)newest.Close();newest=std::move(frame);}
        if(!newest)return;
        struct CloseFrame {Direct3D11CaptureFrame& frame;~CloseFrame(){frame.Close();}} close{newest};
        const auto access=newest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),texture.put_void()));
        D3D11_TEXTURE2D_DESC description{};texture->GetDesc(&description);
        if(!staging||width!=description.Width||height!=description.Height){
            width=description.Width;height=description.Height;
            description.Usage=D3D11_USAGE_STAGING;description.BindFlags=description.MiscFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            staging=nullptr;check_hresult(renderer.device->CreateTexture2D(&description,nullptr,staging.put()));
            pixels.resize(size_t(width)*height*4);
        }
        renderer.context->CopyResource(staging.get(),texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check_hresult(renderer.context->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped));
        for(UINT row=0;row<height;++row)std::copy_n(static_cast<const uint8_t*>(mapped.pData)+size_t(row)*mapped.RowPitch,size_t(width)*4,pixels.data()+size_t(row)*width*4);
        renderer.context->Unmap(staging.get(),0);
        if(renderer.reportedState!=1)return;
        ++frames;unsigned minimum=255,maximum=0,alpha=255;
        for(UINT y=height*3/5;y<height*9/10;y+=8)for(UINT x=24;x+24<width;x+=4){
            const auto* pixel=pixels.data()+(size_t(y)*width+x)*4;
            minimum=std::min(minimum,unsigned(pixel[2]));maximum=std::max(maximum,unsigned(pixel[2]));alpha=std::min(alpha,unsigned(pixel[3]));
        }
        lastRange=double(maximum-minimum);
        if(lastRange<8)++blankReadyFrames;
        if(alpha!=255)++opaqueFailures;
    }
    void save(const std::filesystem::path& path){
        if(pixels.empty())throw std::runtime_error("No presented host frame");
        BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+width*height*4;
        BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(width);info.biHeight=-LONG(height);info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
        std::ofstream image{path,std::ios::binary};image.write(reinterpret_cast<const char*>(&header),sizeof(header));image.write(reinterpret_cast<const char*>(&info),sizeof(info));image.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
        if(!image)throw std::runtime_error("Presented host bitmap");
    }
};
LRESULT CALLBACK runtime_background(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};const HDC dc=BeginPaint(window,&paint);RECT bounds{};GetClientRect(window,&bounds);
        for(int x=0;x<bounds.right;x+=64){RECT stripe{x,0,x+64,bounds.bottom};
            HBRUSH brush=CreateSolidBrush((x/64+runtimePattern)%2?RGB(32,96,180):RGB(208,128,64));FillRect(dc,&stripe,brush);DeleteObject(brush);}
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(240,240,240));
        for(int y=16;y<bounds.bottom;y+=48)TextOutW(dc,12,y,L"TieZ synthetic glass runtime fixture",36);
        EndPaint(window,&paint);return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
void pump_runtime(unsigned milliseconds,RuntimeCapture* monitor=nullptr) {
    const auto until=GetTickCount64()+milliseconds;
    do {MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            const auto begin=std::chrono::steady_clock::now();TranslateMessage(&message);DispatchMessageW(&message);
            runtimeMaxDispatchMs=std::max(runtimeMaxDispatchMs,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
        }
        auto begin=std::chrono::steady_clock::now();if(monitor)monitor->drain();
        runtimeMaxReadbackMs=std::max(runtimeMaxReadbackMs,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
        begin=std::chrono::steady_clock::now();Sleep(1);
        runtimeMaxSleepMs=std::max(runtimeMaxSleepMs,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
    } while(GetTickCount64()<until);
}
int runtime_regression(const wchar_t* reportPath,bool dark=false) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW sourceClass{};sourceClass.lpfnWndProc=runtime_background;sourceClass.hInstance=GetModuleHandleW(nullptr);
    sourceClass.lpszClassName=L"TieZ.Glass.Runtime.Background";
    if(!RegisterClassW(&sourceClass)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("runtime class");
    RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    const int x=work.left+20,y=work.top+20,w=384,h=448;
    HWND source=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,sourceClass.lpszClassName,
        L"TieZ synthetic material background",WS_POPUP,x,y,900,750,nullptr,nullptr,sourceClass.hInstance,nullptr);
    HiddenWindow host;
    SetWindowLongPtrW(host.value,GWL_EXSTYLE,WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TOPMOST);
    SetWindowPos(host.value,HWND_TOPMOST,x+220,y+160,w,h,SWP_NOACTIVATE|SWP_FRAMECHANGED);
    ShowWindow(source,SW_SHOWNOACTIVATE);UpdateWindow(source);ShowWindow(host.value,SW_SHOWNOACTIVATE);
    RuntimeEvents events;TiezGlass* raw=nullptr;
    const HRESULT created=tiez_glass_create(host.value,source,&events,runtime_state,&raw);
    std::unique_ptr<TiezGlass,void(*)(TiezGlass*)> renderer(raw,tiez_glass_destroy);
    TiezGlassControl controls[6]{};
    const float scale=float(GetDpiForWindow(host.value))/96.f;
    for(unsigned i=0;i<6;++i){controls[i].x=12;controls[i].y=12+float(i)*42;controls[i].width=float(w)/scale-24;controls[i].height=32;controls[i].radius=16;}
    TiezGlassState parameters{};parameters.viewport_width=float(w)/scale;parameters.viewport_height=float(h)/scale;
    parameters.blur_radius=8.75f;
    const float light[]{245.f/255,248.f/255,252.f/255,.82f},night[]{23.f/255,31.f/255,43.f/255,.82f};
    std::copy_n(dark?night:light,4,parameters.tint_rgba);
    parameters.controls=controls;parameters.control_count=6;parameters.geometry_revision=1;
    const HRESULT updated=SUCCEEDED(created)?tiez_glass_update(raw,&parameters):created;
    const auto deadline=GetTickCount64()+8000;
    while(SUCCEEDED(updated)&&raw->reportedState!=1&&!raw->failed&&GetTickCount64()<deadline)pump_runtime(10);
    std::ofstream file(reportPath);file<<"{\"mode\":\""<<(dark?"dark":"light")<<"\",\"dpi\":"<<GetDpiForWindow(host.value)<<",\"viewportPixels\":["<<w<<','<<h<<"],\"initialReady\":"<<(raw&&raw->reportedState==1?"true":"false")<<",\"created\":"<<created<<",\"updated\":"<<updated<<",\"phases\":[";
    bool first=true;bool passed=raw&&raw->reportedState==1;
    std::unique_ptr<RuntimeCapture> monitor;
    if(passed){
        monitor=std::make_unique<RuntimeCapture>(*raw);pump_runtime(120,monitor.get());
        monitor->frames=monitor->blankReadyFrames=monitor->opaqueFailures=0;
        for(unsigned scenario=0;scenario<3;++scenario){
            const bool noZOrder=scenario==0;
            WINDOWPOS diagnostic{host.value,HWND_TOP,x+200,y+160,w,h,UINT(SWP_NOSIZE|SWP_NOACTIVATE|(noZOrder?SWP_NOZORDER:0))};
            const auto& order=raw->configuredZOrder;
            std::fprintf(stderr,"[runtime-move-order] nozorder=%d eligible=%d host=%p configured=[%p,%p,%d,%d] actual=[%p,%p,%d,%d]\n",
                noZOrder,raw->position_change_can_retarget(diagnostic),static_cast<void*>(host.value),static_cast<void*>(order.previous),static_cast<void*>(order.next),order.topmost,order.previous_topmost,
                static_cast<void*>(GetWindow(host.value,GW_HWNDPREV)),static_cast<void*>(GetWindow(host.value,GW_HWNDNEXT)),
                (GetWindowLongPtrW(host.value,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0,
                GetWindow(host.value,GW_HWNDPREV)&&(GetWindowLongPtrW(GetWindow(host.value,GW_HWNDPREV),GWL_EXSTYLE)&WS_EX_TOPMOST)!=0);
            const size_t start=events.states.size();double longest=0,total=0,longestIteration=0,totalIteration=0;const unsigned moves=36;
            const uint64_t capture=raw->dispatch->generation,revision=raw->cropRevision;
            runtimeMaxDispatchMs=runtimeMaxReadbackMs=runtimeMaxSleepMs=0;
            SendMessageW(host.value,WM_ENTERSIZEMOVE,0,0);
            for(unsigned step=0;step<moves;++step){
                const auto before=std::chrono::steady_clock::now();
                const int left=scenario==2?x+160+int(step%20)*11:x+200+int(step%12)*8;
                SetWindowPos(host.value,HWND_TOP,left,y+160,w,h,SWP_NOSIZE|SWP_NOACTIVATE|(noZOrder?SWP_NOZORDER:0));
                const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
                longest=std::max(longest,elapsed);total+=elapsed;pump_runtime(12,monitor.get());
                const double iteration=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
                longestIteration=std::max(longestIteration,iteration);totalIteration+=iteration;
            }
            SendMessageW(host.value,WM_EXITSIZEMOVE,0,0);pump_runtime(400,monitor.get());
            const auto withdrawals=std::count(events.states.begin()+start,events.states.end(),0);
            if(!first)file<<',';first=false;
            const bool sameCapture=raw->dispatch&&raw->dispatch->generation==capture;
            file<<"{\"noZOrderFlag\":"<<(noZOrder?"true":"false")<<",\"crossReserve\":"<<(scenario==2?"true":"false")<<",\"moves\":"<<moves<<",\"meanWindowPosMs\":"<<total/moves<<",\"maxWindowPosMs\":"<<longest<<",\"meanIterationMs\":"<<totalIteration/moves<<",\"maxIterationMs\":"<<longestIteration<<",\"maxDispatchMs\":"<<runtimeMaxDispatchMs<<",\"maxVerificationReadbackMs\":"<<runtimeMaxReadbackMs<<",\"maxFixtureSleepMs\":"<<runtimeMaxSleepMs<<",\"sameCapture\":"<<(sameCapture?"true":"false")<<",\"retargets\":"<<raw->cropRevision-revision<<",\"withdrawals\":"<<withdrawals<<",\"finalState\":"<<raw->reportedState<<",\"error\":"<<events.error<<'}';
            passed=passed&&withdrawals==0&&sameCapture&&raw->reportedState==1&&!raw->failed;
        }
        const size_t start=events.states.size();double longest=0,total=0;const unsigned clicks=12;
        for(unsigned step=0;step<clicks;++step){
            const auto before=std::chrono::steady_clock::now();
            SetWindowPos(host.value,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
            longest=std::max(longest,elapsed);total+=elapsed;
            controls[0].pressure=step%2?0.f:1.f;tiez_glass_update(raw,&parameters);
            pump_runtime(35,monitor.get());
        }
        pump_runtime(400,monitor.get());
        const auto withdrawals=std::count(events.states.begin()+start,events.states.end(),0);
        file<<",{\"name\":\"activation_and_press\",\"updates\":"<<clicks<<",\"meanWindowPosMs\":"<<total/clicks<<",\"maxWindowPosMs\":"<<longest<<",\"withdrawals\":"<<withdrawals<<",\"finalState\":"<<raw->reportedState<<",\"error\":"<<events.error<<'}';
        passed=passed&&withdrawals==0&&raw->reportedState==1&&!raw->failed;
        const size_t backgroundStart=events.states.size();
        const auto beforeColor=monitor->pixels[(size_t(monitor->height)*3/4*monitor->width+monitor->width/2)*4+2];
        double change=0;
        for(unsigned update=0;update<24;++update){
            ++runtimePattern;InvalidateRect(source,nullptr,FALSE);UpdateWindow(source);pump_runtime(35,monitor.get());
            const auto color=monitor->pixels[(size_t(monitor->height)*3/4*monitor->width+monitor->width/2)*4+2];
            change=std::max(change,std::abs(double(color)-beforeColor));
        }
        const auto backgroundWithdrawals=std::count(events.states.begin()+backgroundStart,events.states.end(),0);
        file<<",{\"name\":\"dynamic_background\",\"updates\":24,\"presentedColorChange\":"<<change<<",\"withdrawals\":"<<backgroundWithdrawals<<",\"finalState\":"<<raw->reportedState<<'}';
        passed=passed&&change>4&&backgroundWithdrawals==0&&raw->reportedState==1&&!raw->failed;
        const unsigned firstFrames=monitor->frames,blanks=monitor->blankReadyFrames,alpha=monitor->opaqueFailures;
        passed=passed&&firstFrames>0&&blanks==0&&alpha==0;
        file<<"],\"presentedFrames\":"<<firstFrames<<",\"blankReadyFrames\":"<<blanks<<",\"nonopaqueInteriorFrames\":"<<alpha<<",\"recoveries\":[";
        for(unsigned round=0;round<3;++round){
            ShowWindow(host.value,SW_HIDE);pump_runtime(25);ShowWindow(host.value,SW_SHOWNOACTIVATE);
            const auto began=std::chrono::steady_clock::now();
            while(raw->reportedState!=1&&!raw->failed&&std::chrono::steady_clock::now()-began<std::chrono::seconds(3))pump_runtime(10);
            const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
            pump_runtime(100,monitor.get());
            file<<(round?",":"")<<"{\"ready\":"<<(raw->reportedState==1?"true":"false")<<",\"milliseconds\":"<<elapsed<<",\"failed\":"<<(raw->failed?"true":"false")<<'}';
            passed=passed&&raw->reportedState==1&&!raw->failed;
        }
        std::filesystem::path image=reportPath;image.replace_extension(L".bmp");monitor->save(image);
    }
    file<<"],\"passed\":"<<(passed?"true":"false")<<",\"scope\":\"Live WGC and DComp with synthetic windows; SetWindowPos and modal notifications, no mouse or keyboard input\"}";
    monitor.reset();renderer.reset();DestroyWindow(source);return passed?0:1;
}
