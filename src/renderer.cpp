#include "renderer.h"

#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <cwchar>

namespace
{

const wchar_t* kWindowClass = L"WallpaperSystemHost";

const char* kShaderSource = R"(
cbuffer FrameParams : register(b0)
{
    float2 uvScale;
    float2 uvOffset;
};

Texture2D    frameTex  : register(t0);
Texture2D    chromaTex : register(t1);
SamplerState frameSampler : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = i.uv * uvScale + uvOffset;
    float luma = frameTex.Sample(frameSampler, uv).r;
    float2 chroma = chromaTex.Sample(frameSampler, uv).rg;
    float3 yuv = float3(luma, chroma.x, chroma.y);

    // BT.709 limited range.
    float y = (yuv.x * 255.0 - 16.0) * (1.0 / 219.0);
    float u = (yuv.y * 255.0 - 128.0) * (1.0 / 224.0);
    float v = (yuv.z * 255.0 - 128.0) * (1.0 / 224.0);

    float3 rgb;
    rgb.r = y + 1.5748 * v;
    rgb.g = y - 0.1873 * u - 0.4681 * v;
    rgb.b = y + 1.8556 * u;
    return float4(saturate(rgb), 1.0);
}
)";

struct FrameConstants
{
    float uvScale[2];
    float uvOffset[2];
};

static_assert(sizeof(FrameConstants) == 16, "constant buffer must remain 16-byte aligned");

BOOL CALLBACK EnumIconHostProc(HWND hwnd, LPARAM param)
{
    if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr)
    {
        *reinterpret_cast<HWND*>(param) = hwnd;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK EnumFullscreenWorkerProc(HWND hwnd, LPARAM param)
{
    wchar_t className[64] = {};
    if (GetClassNameW(hwnd, className, _countof(className)) == 0)
        return TRUE;
    if (wcscmp(className, L"WorkerW") != 0)
        return TRUE;
    if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr)
        return TRUE;
    if (!IsWindowVisible(hwnd))
        return TRUE;

    RECT rect = {};
    if (!GetWindowRect(hwnd, &rect))
        return TRUE;
    if (rect.right - rect.left < 640 || rect.bottom - rect.top < 480)
        return TRUE;

    *reinterpret_cast<HWND*>(param) = hwnd;
    return FALSE;
}

HMONITOR PrimaryMonitor()
{
    return MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
}

RECT PrimaryMonitorRect()
{
    RECT rect = { 0, 0, 1920, 1080 };
    MONITORINFO info = { sizeof(info) };
    if (GetMonitorInfoW(PrimaryMonitor(), &info))
        rect = info.rcMonitor;
    return rect;
}

// The adapter driving the primary monitor, so decode + render + scanout share one GPU.
// The adapter that physically scans out the primary monitor. On hybrid laptops
// DXGI reports the panel's outputs on whichever GPU the process runs on, so ask
// the kernel which adapter really owns the display.
ComPtr<IDXGIAdapter1> FindPrimaryMonitorAdapter()
{
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(PrimaryMonitor(), &info))
        return nullptr;

    D3DKMT_OPENADAPTERFROMHDC open = {};
    open.hDc = CreateDCW(info.szDevice, nullptr, nullptr, nullptr);
    if (!open.hDc)
        return nullptr;
    const NTSTATUS status = D3DKMTOpenAdapterFromHdc(&open);
    DeleteDC(open.hDc);
    if (status < 0)
        return nullptr;
    D3DKMT_CLOSEADAPTER close = {};
    close.hAdapter = open.hAdapter;
    D3DKMTCloseAdapter(&close);

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(open.AdapterLuid, IID_PPV_ARGS(&adapter))))
        return nullptr;
    return adapter;
}

// After sending 0x052C to Progman, Explorer creates a fresh top-level WorkerW
// that sits directly behind the window hosting the desktop icons. That window
// owns the desktop wallpaper, so our video window must live inside it.
HWND FindWallpaperWorkerWindow()
{
    HWND iconHost = nullptr;
    EnumWindows(EnumIconHostProc, reinterpret_cast<LPARAM>(&iconHost));

    if (iconHost)
    {
        for (HWND window = GetWindow(iconHost, GW_HWNDNEXT);
             window != nullptr;
             window = GetWindow(window, GW_HWNDNEXT))
        {
            wchar_t className[64] = {};
            if (GetClassNameW(window, className, _countof(className)) &&
                wcscmp(className, L"WorkerW") == 0)
            {
                return window;
            }
        }
    }

    HWND fallback = nullptr;
    EnumWindows(EnumFullscreenWorkerProc, reinterpret_cast<LPARAM>(&fallback));
    return fallback;
}

} // namespace

Renderer::Renderer() = default;

Renderer::~Renderer()
{
    Destroy();
}

bool Renderer::Create(HINSTANCE instance)
{
    failureStage_ = 0;
    failureCode_ = 0;

    instance_ = instance;
    UpdateSizeFromMonitor();

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Renderer::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = nullptr;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        failureStage_ = 1;
        failureCode_ = GetLastError();
        return false;
    }

    hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                            kWindowClass, L"Wallpaper", WS_POPUP,
                            monitorRect_.left, monitorRect_.top, width_, height_,
                            nullptr, nullptr, instance, nullptr);
    if (!hwnd_)
    {
        failureStage_ = 2;
        failureCode_ = GetLastError();
        return false;
    }

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    if (!AttachToDesktop())
    {
        failureStage_ = 3;
        failureCode_ = GetLastError();
        return false;
    }
    if (!CreateDevice())
    {
        failureStage_ = 4;
        failureCode_ = GetLastError();
        return false;
    }
    if (!CreateSwapChain(width_, height_))
    {
        failureStage_ = 5;
        failureCode_ = GetLastError();
        return false;
    }

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    return true;
}

void Renderer::Destroy()
{
    if (frameLatencyWaitable_)
    {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }

    ReleaseBackBuffer();
    cb_.Reset();
    sampler_.Reset();
    ps_.Reset();
    vs_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();

    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void Renderer::ReleaseBackBuffer()
{
    rtv_.Reset();
    backBuffer_.Reset();
    if (context_)
    {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->Flush();
    }
}

void Renderer::UpdateSizeFromMonitor()
{
    monitorRect_ = PrimaryMonitorRect();
    width_ = monitorRect_.right - monitorRect_.left;
    height_ = monitorRect_.bottom - monitorRect_.top;
}

bool Renderer::CheckDisplayChanged()
{
    if (displayChanged_)
        return true;

    const DWORD now = GetTickCount();
    if (now - lastDisplayPoll_ < 1000)
        return false;
    lastDisplayPoll_ = now;

    const RECT current = PrimaryMonitorRect();
    if (!EqualRect(&current, &monitorRect_))
        displayChanged_ = true;

    return displayChanged_;
}

bool Renderer::AttachToDesktop()
{
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman)
    {
        DWORD_PTR result = 0;
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
        if (!FindWallpaperWorkerWindow())
            SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
    }

    HWND parent = FindWallpaperWorkerWindow();
    if (!parent)
        parent = progman;
    if (!parent)
        return false;

    if (!SetParent(hwnd_, parent))
        return false;

    workerW_ = parent;

    RECT target = monitorRect_;
    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<LPPOINT>(&target), 2);

    SetWindowPos(hwnd_, HWND_BOTTOM,
                 target.left, target.top, width_, height_,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return true;
}

bool Renderer::EnsureDesktopParent()
{
    if (workerW_ && IsWindow(workerW_))
        return true;
    return AttachToDesktop();
}

bool Renderer::CreateDevice()
{
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    const UINT videoFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const UINT plainFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

    // A null adapter means the system default.
    auto create = [&](IDXGIAdapter* adapter)
    {
        const D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
        HRESULT result = D3D11CreateDevice(adapter, type, nullptr, videoFlags,
                                           levels, _countof(levels), D3D11_SDK_VERSION,
                                           &device_, &obtained, &context_);
        videoSupport_ = SUCCEEDED(result);
        if (FAILED(result))
        {
            result = D3D11CreateDevice(adapter, type, nullptr, plainFlags,
                                       levels, _countof(levels), D3D11_SDK_VERSION,
                                       &device_, &obtained, &context_);
        }
        return result;
    };

    const ComPtr<IDXGIAdapter1> adapter = FindPrimaryMonitorAdapter();
    HRESULT hr = adapter ? create(adapter.Get()) : E_FAIL;
    if (FAILED(hr) || !videoSupport_)
        hr = create(nullptr);  // Prefer a default adapter that can decode video.
    if (FAILED(hr))
    {
        failureCode_ = static_cast<DWORD>(hr);
        return false;
    }

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);

    return true;
}

bool Renderer::CreateSwapChain(int width, int height)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice)))
        return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)))
        return false;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    HRESULT hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain_);
    if (SUCCEEDED(hr))
    {
        swapChainFlags_ = desc.Flags;
        ComPtr<IDXGISwapChain2> swapChain2;
        if (SUCCEEDED(swapChain_.As(&swapChain2)))
        {
            frameLatencyWaitable_ = swapChain2->GetFrameLatencyWaitableObject();
            swapChain2->SetMaximumFrameLatency(1);
        }
    }
    else
    {
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = 0;
        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain_);
        if (FAILED(hr))
        {
            failureCode_ = static_cast<DWORD>(hr);
            return false;
        }
        swapChainFlags_ = 0;
    }

    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    if (!CreateBackBufferView())
        return false;

    bufferWidth_ = width;
    bufferHeight_ = height;
    return true;
}

bool Renderer::CreateBackBufferView()
{
    // Flip model: buffer 0 always refers to the current back buffer, so the
    // texture and views on it stay valid across Present calls.
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_))))
        return false;
    if (FAILED(device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, &rtv_)))
        return false;
    return true;
}

// Software frames only, so the hardware path never loads d3dcompiler_47.dll
// (delay loaded). ps_ is created last: non-null means everything is ready.
bool Renderer::CreateShaders()
{
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&samplerDesc, &sampler_)))
        return false;

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = sizeof(FrameConstants);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bufferDesc, nullptr, &cb_)))
        return false;

    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    const size_t sourceLength = std::strlen(kShaderSource);

    ComPtr<ID3DBlob> vertexBlob;
    ComPtr<ID3DBlob> pixelBlob;
    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompile(kShaderSource, sourceLength, nullptr, nullptr, nullptr,
                            "VSMain", "vs_5_0", flags, 0, &vertexBlob, &errors);
    if (FAILED(hr))
    {
        failureCode_ = static_cast<DWORD>(hr);
        return false;
    }

    errors.Reset();

    hr = D3DCompile(kShaderSource, sourceLength, nullptr, nullptr, nullptr,
                    "PSMain", "ps_5_0", flags, 0, &pixelBlob, &errors);
    if (FAILED(hr))
    {
        failureCode_ = static_cast<DWORD>(hr);
        return false;
    }

    if (FAILED(device_->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                            vertexBlob->GetBufferSize(), nullptr, &vs_)))
        return false;
    if (FAILED(device_->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                          pixelBlob->GetBufferSize(), nullptr, &ps_)))
        return false;

    return true;
}

void Renderer::WaitForFrameLatency()
{
    if (frameLatencyWaitable_)
        WaitForSingleObject(frameLatencyWaitable_, 1000);
}

HRESULT Renderer::RenderFrame(ID3D11ShaderResourceView* srv, ID3D11ShaderResourceView* srvUV,
                              int videoWidth, int videoHeight)
{
    if (!srv || !srvUV || !rtv_ || !context_)
        return E_FAIL;

    // failureStage_ stops a failed shader setup from being retried every frame.
    if (!ps_ && (failureStage_ != 0 || !CreateShaders()))
    {
        failureStage_ = 6;
        return E_FAIL;
    }

    FrameConstants constants = {};
    const float srcWidth = static_cast<float>(videoWidth);
    const float srcHeight = static_cast<float>(videoHeight);
    const float dstWidth = static_cast<float>(width_);
    const float dstHeight = static_cast<float>(height_);

    if (srcWidth > 0.0f && srcHeight > 0.0f)
    {
        // Cover: fill the screen and center-crop the excess.
        const float scale = (std::max)(dstWidth / srcWidth, dstHeight / srcHeight);
        const float visibleWidth = dstWidth / scale;
        const float visibleHeight = dstHeight / scale;
        constants.uvScale[0] = visibleWidth / srcWidth;
        constants.uvScale[1] = visibleHeight / srcHeight;
        constants.uvOffset[0] = (1.0f - constants.uvScale[0]) * 0.5f;
        constants.uvOffset[1] = (1.0f - constants.uvScale[1]) * 0.5f;
    }
    else
    {
        constants.uvScale[0] = 1.0f;
        constants.uvScale[1] = 1.0f;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return E_FAIL;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(cb_.Get(), 0);

    ID3D11RenderTargetView* renderTarget = rtv_.Get();
    context_->OMSetRenderTargets(1, &renderTarget, nullptr);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = dstWidth;
    viewport.Height = dstHeight;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);

    ID3D11ShaderResourceView* views[2] = { srv, srvUV };
    context_->PSSetShaderResources(0, 2, views);

    ID3D11Buffer* constantBuffer = cb_.Get();
    ID3D11SamplerState* sampler = sampler_.Get();

    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &constantBuffer);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->Draw(3, 0);
    return S_OK;
}

bool Renderer::HandleDisplayChange()
{
    displayChanged_ = false;
    UpdateSizeFromMonitor();

    RECT target = monitorRect_;
    if (workerW_ && IsWindow(workerW_))
        MapWindowPoints(HWND_DESKTOP, workerW_, reinterpret_cast<LPPOINT>(&target), 2);

    SetWindowPos(hwnd_, HWND_BOTTOM,
                 target.left, target.top, width_, height_,
                 SWP_NOACTIVATE);

    if (!swapChain_ || (width_ == bufferWidth_ && height_ == bufferHeight_))
        return true;

    ReleaseBackBuffer();
    const HRESULT hr = swapChain_->ResizeBuffers(0,
                                                 static_cast<UINT>(width_),
                                                 static_cast<UINT>(height_),
                                                 DXGI_FORMAT_UNKNOWN,
                                                 swapChainFlags_);
    if (FAILED(hr))
        return false;
    if (!CreateBackBufferView())
        return false;

    bufferWidth_ = width_;
    bufferHeight_ = height_;
    return true;
}

LRESULT CALLBACK Renderer::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    Renderer* renderer = reinterpret_cast<Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_DISPLAYCHANGE:
        if (renderer)
            renderer->displayChanged_ = true;
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
