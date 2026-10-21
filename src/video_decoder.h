#pragma once

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <string>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct DecodedFrame
{
    bool hardware = false;                      // On the GPU: draw it with VideoDecoder::BlitTo.
    ID3D11ShaderResourceView* srv = nullptr;    // Luma, software path only.
    ID3D11ShaderResourceView* srvUV = nullptr;  // Chroma, software path only.
};

enum class DecodeResult
{
    Frame,
    EndOfStream,
    Retry
};

// Decodes an H.264 .mp4 through Media Foundation. The hardware path uses
// DXVA and keeps the frame on the GPU; the software fallback uploads NV12.
class VideoDecoder
{
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool Open(const std::wstring& path, ID3D11Device* device);
    void Close();

    DecodeResult DecodeNext(DecodedFrame& frame);
    bool Rewind();

    // Hardware frames: scales the current frame (cover crop) into target.
    HRESULT BlitTo(ID3D11Texture2D* target, int targetWidth, int targetHeight);
    void ReleaseOutputTarget();

    bool hardware() const { return hardware_; }
    int width() const { return width_; }
    int height() const { return height_; }
    LONGLONG timestamp() const { return timestamp_; }
    int hardwareFailureStage() const { return hwStage_; }
    long hardwareFailureCode() const { return hwCode_; }

private:
    bool OpenHardware();
    bool OpenSoftware();
    bool CreateReaderHardware(const std::wstring& url);
    bool CreateReaderSoftware(const std::wstring& url);
    bool NegotiateOutputType(const GUID& subtype);
    bool ReadOutputType();
    bool PrepareHardwareResources();
    bool PrepareSoftwareResources();
    bool UploadSoftwareFrame(const BYTE* data, LONG pitch, DWORD bufferLength);
    bool DecodeHardware(IMFSample* sample, DecodedFrame& frame);
    bool DecodeSoftware(IMFSample* sample, DecodedFrame& frame);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IMFDXGIDeviceManager> d3dManager_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnumerator_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<IMFSample> currentSample_;  // Keeps the decoder surface alive until the blit.
    ComPtr<ID3D11Texture2D> inputTexture_;
    std::vector<ComPtr<ID3D11VideoProcessorInputView>> inputViews_;  // Per subresource of inputTexture_.
    ID3D11VideoProcessorInputView* currentInput_ = nullptr;
    ComPtr<ID3D11VideoProcessorOutputView> outputView_;
    ID3D11Texture2D* outputTarget_ = nullptr;
    ComPtr<ID3D11Texture2D> yTexture_;
    ComPtr<ID3D11ShaderResourceView> yView_;
    ComPtr<ID3D11Texture2D> uvTexture_;
    ComPtr<ID3D11ShaderResourceView> uvView_;
    std::wstring url_;
    bool hardware_ = false;
    int width_ = 0;        // Visible size.
    int height_ = 0;
    int codedWidth_ = 0;   // Decoded surface size.
    int codedHeight_ = 0;
    int apertureX_ = 0;    // Visible region offset in the decoded surface.
    int apertureY_ = 0;
    LONGLONG timestamp_ = 0;
    int hwStage_ = 0;
    long hwCode_ = 0;
};
