#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Owns the wallpaper window, the D3D11 device and the swap chain. The window
// is parented to Explorer's WorkerW so it renders behind the desktop icons.
class Renderer
{
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Create(HINSTANCE instance);
    void Destroy();

    int failureStage() const { return failureStage_; }
    DWORD failureCode() const { return failureCode_; }
    bool hasVideoSupport() const { return videoSupport_; }

    HWND window() const { return hwnd_; }
    HWND workerWindow() const { return workerW_; }
    ID3D11Device* device() const { return device_.Get(); }

    void WaitForFrameLatency();
    HRESULT RenderFrame(ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* srvUV,
                        int videoWidth, int videoHeight, bool planarUV);

    bool CheckDisplayChanged();
    bool HandleDisplayChange();
    bool EnsureDesktopParent();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateDevice();
    bool CreateSwapChain(int width, int height);
    bool CreateBackBufferView();
    bool CompileShaders();
    bool AttachToDesktop();
    void UpdateSizeFromMonitor();
    void ReleaseBackBuffer();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND workerW_ = nullptr;
    RECT monitorRect_ = {};
    int width_ = 0;
    int height_ = 0;
    int bufferWidth_ = 0;
    int bufferHeight_ = 0;
    UINT swapChainFlags_ = 0;
    bool displayChanged_ = false;
    DWORD lastDisplayPoll_ = 0;
    int failureStage_ = 0;
    DWORD failureCode_ = 0;
    bool videoSupport_ = false;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> sampler_;
    HANDLE frameLatencyWaitable_ = nullptr;
};
