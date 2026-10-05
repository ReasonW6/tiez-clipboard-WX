// No HWND or GPU. Exercise the production reserve/copy geometry using an
// independent world-coordinate oracle, including limits and negative origins.
#include "../../src-tauri/native/glass_capture_region.h"
#include <cstdio>
#include <cstdlib>
#include <array>
#include <limits>

using namespace tiez_glass_detail;
namespace {
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
uint64_t world(int32_t x,int32_t y){return uint64_t(uint32_t(y))<<32|uint32_t(x);}
void check_copy(PixelRect source,PixelRect target){
    ResizeCopy copy{};
    require(capture_viewport_copy(source,target,copy),"an entirely captured viewport can be copied");
    require(copy.destination_x==0&&copy.destination_y==0,"reserve extraction starts at viewport origin");
    for(uint32_t y=0;y<copy.height;++y)for(uint32_t x=0;x<copy.width;++x){
        const auto sampled=world(source.left+int32_t(copy.source_x+x),source.top+int32_t(copy.source_y+y));
        const auto expected=world(target.left+int32_t(x),target.top+int32_t(y));
        require(sampled==expected,"copy preserves the screen coordinate of every pixel");
    }
}
}
int main(){
    for(float dpi:{1.f,1.25f,1.5f,2.f,3.f})for(float sigma:{1.5f,8.75f,16.f,128.f}){
        const auto padding=capture_padding(dpi,sigma*dpi);
        require(padding>=uint32_t(std::ceil(64*dpi)+std::ceil(3*std::min(sigma*dpi,128.f)))+1,
            "padding includes the real expansion budget and complete filter support");
    }
    for(float bad:{0.f,-1.f,17.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        require(!capture_padding(bad,16),"invalid DPI cannot propose an unbounded allocation");
    require(!capture_padding(1,-1)&&!capture_padding(1,std::numeric_limits<float>::quiet_NaN()),"invalid sigma is rejected");

    const PixelRect available{-1920,-1080,1920,1080};
    for(PixelRect client:std::array<PixelRect,5>{{{20,30,180,150},{-600,-350,-440,-230},
        {-1920,-1080,-1760,-960},{1760,960,1920,1080},{-20,-30,140,90}}}){
        PixelRect reserve{};
        require(capture_region(client,available,113,reserve),"bounded reserve selection succeeds");
        require(contains_rect(reserve,client)&&contains_rect(available,reserve),"reserve contains client and remains in the source");
        require(reserve.right-reserve.left==386&&reserve.bottom-reserve.top==346,
            "near an edge the fixed extent shifts inward instead of changing capture dimensions");
        check_copy(reserve,client);
        for(int delta:{14,15,64})for(unsigned side=0;side<8;++side){
            auto next=client;
            if(side==0||side==4||side==6)next.left-=delta;
            if(side==1||side==5||side==7)next.right+=delta;
            if(side==2||side==4||side==5)next.top-=delta;
            if(side==3||side==6||side==7)next.bottom+=delta;
            if(contains_rect(reserve,next))check_copy(reserve,next);
            else{ResizeCopy copy{9,8,7,6,5,4};require(!capture_viewport_copy(reserve,next,copy)&&copy.width==5,
                "uncaptured expansion is rejected without inventing a copy");}
        }
    }
    PixelRect result{}, small{-8,-5,42,35},client{0,0,30,20};
    require(capture_region(client,small,113,result)&&same_rect(result,small),"small authorized sources bound the entire reserve");
    const PixelRect offscreen{-40,0,20,20};
    require(capture_region(offscreen,small,113,result)&&same_rect(result,offscreen),
        "a partly unavailable client keeps exact legacy extent, never a fake reserve");
    require(capture_region(client,small,0,result)&&same_rect(result,client),"explicit zero reserve keeps exact viewport");
    for(PixelRect bad:std::array<PixelRect,4>{{{1,0,1,5},{0,1,5,1},{0,0,8193,20},{INT32_MIN,0,INT32_MAX,20}}}){
        result={1,2,3,4};require(!capture_region(bad,available,113,result)&&same_rect(result,{1,2,3,4}),
            "empty or overflowing clients cannot publish a partial result");
    }
    const PixelRect worldLimit{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX};
    for(PixelRect corner:std::array<PixelRect,2>{{{INT32_MIN,INT32_MIN,INT32_MIN+8192,INT32_MIN+8192},
        {INT32_MAX-8192,INT32_MAX-8192,INT32_MAX,INT32_MAX}}}){
        require(capture_region(corner,worldLimit,8192,result)&&contains_rect(result,corner)&&
            int64_t(result.right)-result.left==16384&&int64_t(result.bottom)-result.top==16384,
            "maximum reserve remains representable at both signed-coordinate limits");
    }
    ResizeCopy copy{};
    require(!capture_viewport_copy(worldLimit,client,copy),"impossible source texture extent is rejected before copying");
    require(!capture_viewport_copy({0,0,16384,16384},{0,0,8193,10},copy),"viewport size retains its independent limit");
    const auto combined=union_rect({-400,-300,-20,50},{-25,-10,140,120});
    require(same_rect(combined,{-400,-300,140,120}),"pending resize validates both the old captured source and new client");

    const PixelRect centered{0,0,160,120};
    for(float sigma:{0.f,1.5f,8.75f,16.f,32.f,128.f,256.f}){
        const int halo=int(std::ceil(3*std::min(sigma,128.f)))+1;
        const PixelRect supported{-halo,-halo,160+halo,120+halo};
        require(capture_supports(supported,centered,available,sigma),"complete physical filter support permits viewport reuse");
        for(unsigned edge=0;edge<4;++edge){
            auto missing=supported;
            if(edge==0)++missing.left;else if(edge==1)++missing.top;
            else if(edge==2)--missing.right;else --missing.bottom;
            require(!capture_supports(missing,centered,available,sigma),
                "capturing the viewport without the final filter texel cannot retain its session");
        }
    }
    require(capture_supports(small,client,small,128),"a real source boundary needs no invented outside halo");
    require(!capture_supports(small,offscreen,small,0),"off-source pixels cannot be promoted into reserve coverage");
    for(float invalid:{-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        require(!capture_supports(available,centered,available,invalid),"invalid filter support rejects session reuse");
    for(PixelRect corner:std::array<PixelRect,2>{{{INT32_MIN,INT32_MIN,INT32_MIN+160,INT32_MIN+120},
        {INT32_MAX-160,INT32_MAX-120,INT32_MAX,INT32_MAX}}}){
        require(capture_region(corner,worldLimit,449,result)&&capture_supports(result,corner,worldLimit,128),
            "halo arithmetic remains valid at both signed-coordinate limits");
    }

    const PixelRect a{10,20,435,609},b{10,20,290,609},moved{110,220,535,809};
    require(capture_viewport_can_translate(a,moved,425,589,false,false),"an unchanged actual extent can retarget its position");
    require(capture_viewport_can_translate(a,moved,0,0,true,false),"NOSIZE ignores request dimensions for a stable viewport");
    require(!capture_viewport_can_translate(a,a,280,589,false,false),"a new size enters the resize handoff");
    for(bool pending:{false,true}){
        require(!capture_viewport_can_translate(a,b,425,589,false,pending),
            "A-to-B-to-A cannot bypass a fresh size generation by matching the stale A snapshot");
        require(!capture_viewport_can_translate(a,b,0,0,true,pending),
            "NOSIZE cannot make a stale output snapshot authoritative");
    }
    require(!capture_viewport_can_translate(a,a,425,589,false,true),"pending output reconciliation cannot masquerade as a translation");
    std::printf("PASS: %u capture-region checks; synthetic coordinates, no HWND or desktop access.\n",checks);
    return 0;
}
