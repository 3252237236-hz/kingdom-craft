#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <reshade.hpp>
#include <vector>
#include <cstdio>
#include <cmath>

using Microsoft::WRL::ComPtr;
using namespace reshade::api;
constexpr UINT W = 320, H = 180;

int main()
{
    SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");
    const auto module = LoadLibraryW(L"ReShade64.dll");
    if (!module) return 1;
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SkyCraftHiddenShaderTest";
    RegisterClassW(&wc);
    const auto window = CreateWindowW(wc.lpszClassName, L"SkyCraft shader test", WS_OVERLAPPEDWINDOW,
        0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr); // deliberately never shown
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=1; sd.BufferDesc.Width=W; sd.BufferDesc.Height=H;
    sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count=1;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=window; sd.Windowed=TRUE;
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; ComPtr<IDXGISwapChain> swap;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&sd,&swap,&dev,nullptr,&ctx))) return 2;
    effect_runtime* runtime=nullptr;
    if (!reshade::create_effect_runtime(device_api::d3d11,dev.Get(),ctx.Get(),swap.Get(),"ReShade.ini",&runtime)) return 3;
    effect_technique technique{};
    const auto start=GetTickCount64();
    while (GetTickCount64()-start<30000) {
        reshade::update_and_present_effect_runtime(runtime);
        technique=runtime->find_technique("SkyCraftKCD2.fx","SkyCraftKCD2");
        if (technique.handle) break;
        MSG msg; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&msg); DispatchMessageW(&msg);}
        Sleep(25);
    }
    if (!technique.handle) { std::puts("SHADER_RUNTIME_RESULT: FAILED compilation"); return 4; }
    std::puts("SHADER_RUNTIME_PASS: effect compiles with ReShade 6.8.0");
    ComPtr<ID3D11Texture2D> back; swap->GetBuffer(0,IID_PPV_ARGS(&back));
    ComPtr<ID3D11RenderTargetView> rtv; dev->CreateRenderTargetView(back.Get(),nullptr,&rtv);
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    std::vector<ComPtr<ID3D11ShaderResourceView>> views;
    const auto texture=[&](DXGI_FORMAT format,const void* data,const char* semantic) {
        D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=1;d.ArraySize=1;
        d.Format=format;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;initial.SysMemPitch=W*4;
        ComPtr<ID3D11Texture2D> tex;ComPtr<ID3D11ShaderResourceView> srv;
        if(FAILED(dev->CreateTexture2D(&d,&initial,&tex)) || FAILED(dev->CreateShaderResourceView(tex.Get(),nullptr,&srv))) return false;
        const resource_view v{reinterpret_cast<uint64_t>(srv.Get())};
        runtime->update_texture_bindings(semantic,v,v);
        textures.push_back(tex);views.push_back(srv);return true;
    };
    std::vector<uint32_t> world(W*H,0xFFFF00FF), overlay(W*H,0);
    for (UINT y=0;y<20;y++) for(UINT x=0;x<20;x++) overlay[y*W+x]=0xFF00FFFF;
    const auto reversed=[](float z,float n,float f) {return (n*f/z-n)/(f-n);};
    std::vector<float> mcDepth(W*H,reversed(3,.05f,2048));
    std::vector<float> hostDepth(W*H,reversed(2,.1f,1000));
    if(!texture(DXGI_FORMAT_R8G8B8A8_UNORM,world.data(),"MCWORLD")
        || !texture(DXGI_FORMAT_R32_FLOAT,mcDepth.data(),"MCDEPTH")
        || !texture(DXGI_FORMAT_R8G8B8A8_UNORM,overlay.data(),"MCOVERLAY")
        || !texture(DXGI_FORMAT_R32_FLOAT,hostDepth.data(),"DEPTH")) return 5;
    const auto boolean=[&](const char* name,bool value) {runtime->set_uniform_value_bool(runtime->find_uniform_variable("SkyCraftKCD2.fx",name),value);};
    const auto scalar=[&](const char* name,float value) {runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx",name),value);};
    boolean("McActive",true);boolean("HostProjectionReady",true);boolean("Reproject",false);
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","HostPlanes"),.1f,1000);
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","McPlanes"),.05f,2048,7);
    for(const auto name:{"LightMatch","GradeMatch","HazeStrength","EdgeSoftness","ContactShadow","BloomStrength"}) scalar(name,0);
    scalar("DepthBias",0);scalar("SlopeBias",0);
    boolean("CursorVisible",false);
    std::vector<uint8_t> pixels(W*H*4);
    const auto draw=[&](bool ready,bool projection) {
        boolean("HostDepthReady",ready);boolean("HostProjectionReady",projection);
        const float background[]={0.1f,0.2f,0.4f,1};ctx->ClearRenderTargetView(rtv.Get(),background);
        const resource_view target{reinterpret_cast<uint64_t>(rtv.Get())};
        runtime->render_technique(technique,runtime->get_command_queue()->get_immediate_command_list(),target,target);
        return runtime->capture_screenshot(pixels.data());
    };
    const auto pixel=[&](UINT x,UINT y,UINT channel){return pixels[(y*W+x)*4+channel];};
    const auto hostVisible=[&](){return pixel(W/2,H/2,0)<60 && pixel(W/2,H/2,2)>70 && pixel(W/2,H/2,2)<140;};
    const auto overlayVisible=[&](){return pixel(5,H-6,0)>200 && pixel(5,H-6,1)>200 && pixel(5,H-6,2)<30;};
    bool passed=draw(true,true) && hostVisible();
    std::printf("SHADER_RUNTIME_%s: foreground host surface hides MC world\n",passed?"PASS":"FAIL");
    const bool absent=draw(false,true) && hostVisible() && overlayVisible();
    passed &= absent;
    std::printf("SHADER_RUNTIME_%s: missing native depth hides world, preserves HUD\n",absent?"PASS":"FAIL");
    std::fill(hostDepth.begin(),hostDepth.end(),reversed(4,.1f,1000));
    ctx->UpdateSubresource(textures[3].Get(),0,nullptr,hostDepth.data(),W*4,0);
    const bool foreground=draw(true,true) && pixel(W/2,H/2,0)>200 && pixel(W/2,H/2,2)>200;
    passed &= foreground;
    std::printf("SHADER_RUNTIME_%s: nearer MC world is visible\n",foreground?"PASS":"FAIL");
    const bool projection=draw(true,false) && pixel(W/2,H/2,0)>200 && pixel(W/2,H/2,2)>200;
    passed &= projection;
    std::printf("SHADER_RUNTIME_%s: fallback planes preserve visible MC world\n",projection?"PASS":"FAIL");
    unsigned checks = 5;
    const auto check=[&](bool result,const char* label) {
        ++checks; passed &= result;
        std::printf("SHADER_RUNTIME_%s: %s\n",result?"PASS":"FAIL",label);
    };
    const auto uploadWorld=[&]() {ctx->UpdateSubresource(textures[0].Get(),0,nullptr,world.data(),W*4,0);};
    scalar("HazeStart",0);scalar("HazeDistance",10);scalar("HazeStrength",.85f);
    std::fill(world.begin(),world.end(),0);uploadWorld();
    check(draw(true,true) && hostVisible(),"zero-alpha MC has no haze sheet over native ground");
    std::fill(world.begin(),world.end(),0x80000080);uploadWorld();
    const float h=(1-std::exp(-3.f/10))*.85f,a=128.f/255;
    const float expected[]={a*(1-h)+.1f*(1-a),a*.2f*h+.2f*(1-a),a*.4f*h+.4f*(1-a)};
    bool partial=draw(true,true);
    for(UINT i=0;i<3;i++) partial &= std::abs(pixel(W/2,H/2,i)-expected[i]*255)<5;
    check(partial,"partial coverage is relit as straight colour and premultiplied once");
    scalar("HazeStrength",0);
    std::fill(world.begin(),world.end(),0xFFFF00FF);uploadWorld();
    for(UINT y=0;y<H;y++) for(UINT x=0;x<W;x++) hostDepth[y*W+x]=reversed(x<W/2?2.f:4.f,.1f,1000);
    ctx->UpdateSubresource(textures[3].Get(),0,nullptr,hostDepth.data(),W*4,0);
    scalar("EdgeSoftness",1);
    check(draw(true,true) && pixel(W/2-1,H/2,0)<45 && pixel(W/2+1,H/2,0)>200,
        "edge antialiasing does not leak blocks over a nearer native silhouette");
    scalar("EdgeSoftness",0);
    std::fill(hostDepth.begin(),hostDepth.end(),reversed(4,.1f,1000));
    ctx->UpdateSubresource(textures[3].Get(),0,nullptr,hostDepth.data(),W*4,0);
    boolean("Reproject",true);
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","WarpTan"),.7f,.7f,float(W)/H);
    check(draw(true,true) && pixel(W/2,H/2,0)>200 && pixel(W/2,H/2,2)>200,
        "identity reprojection preserves MC foreground depth");
    for(UINT y=0;y<H;y++) for(UINT x=0;x<W;x++) world[y*W+x]=x<W/2?0xFF0000FF:0xFF00FF00;
    uploadWorld();
    const float angle=.3f,c=std::cos(angle),s=std::sin(angle);
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","WarpRow0"),c,0,s);
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","WarpRow2"),-s,0,c);
    check(draw(true,true) && pixel(W/2,H/2,0)>200 && pixel(W/2,H/2,1)<30,
        "rotation-only inverse projection samples the correct source side");
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","WarpRow0"),1,0,0);
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","WarpRow2"),0.f,0.f,1.f);
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","WarpT"),.2f,0,0);
    check(draw(true,true) && pixel(W/2,H/2,1)>200 && pixel(W/2,H/2,0)<30,
        "translated camera converges to the correct source surface");
    std::fill(hostDepth.begin(),hostDepth.end(),reversed(2,.1f,1000));
    ctx->UpdateSubresource(textures[3].Get(),0,nullptr,hostDepth.data(),W*4,0);
    check(draw(true,true) && hostVisible(),"translated MC remains behind native foreground");
    // A one-metre patch on a plane six metres away must move by exactly the
    // perspective projection of a two-metre camera translation.
    std::fill(hostDepth.begin(),hostDepth.end(),reversed(20,.1f,1000));
    std::fill(mcDepth.begin(),mcDepth.end(),reversed(6,.05f,2048));
    ctx->UpdateSubresource(textures[3].Get(),0,nullptr,hostDepth.data(),W*4,0);
    ctx->UpdateSubresource(textures[1].Get(),0,nullptr,mcDepth.data(),W*4,0);
    for(UINT y=0;y<H;y++) for(UINT x=0;x<W;x++) world[y*W+x]=x>=140 && x<170?0xFF0000FF:0xFF00FF00;
    uploadWorld();
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","WarpT"),0.f,0.f,0.f);
    bool baseline=draw(true,true);
    int original=-1;
    for(UINT x=1;x<W-1;x++) if(pixel(x,H/2,0)>200) { original=int(x);break; }
    runtime->set_uniform_value_float(runtime->find_uniform_variable("SkyCraftKCD2.fx","WarpT"),2.f,0.f,0.f);
    bool moved=draw(true,true);
    int translated=-1;
    for(UINT x=1;x<W-1;x++) if(pixel(x,H/2,0)>200) { translated=int(x);break; }
    const float expectedShift=float(H)*2.f/(2.f*.7f*6.f);
    check(baseline && moved && original>=0 && translated>=0
        && std::abs((original-translated)-expectedShift)<1.5f,
        "two-metre lateral camera motion yields the same two-metre block parallax");
    std::printf("SHADER_RUNTIME_CHECKS: %u\n",checks);
    std::printf("SHADER_RUNTIME_RESULT: %s\n",passed?"PASSED":"FAILED");
    reshade::destroy_effect_runtime(runtime);
    DestroyWindow(window);
    return passed?0:6;
}
