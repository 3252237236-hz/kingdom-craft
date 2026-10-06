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
    std::printf("SHADER_RUNTIME_RESULT: %s\n",passed?"PASSED":"FAILED");
    reshade::destroy_effect_runtime(runtime);
    DestroyWindow(window);
    return passed?0:6;
}
