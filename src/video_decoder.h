#pragma once

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct DecodedFrame
{
    ID3D11ShaderResourceView* srv = nullptr;    // BGRA view, or luma for the software path.
    ID3D11ShaderResourceView* srvUV = nullptr;  // Chroma view, software path only.
    bool planarUV = false;
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
    bool PrepareHardwareResources();
    bool PrepareSoftwareResources();
    bool UploadSoftwareFrame(const BYTE* data, int pitch, DWORD bufferLength);
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
    ComPtr<ID3D11Texture2D> bgraTexture_;
    ComPtr<ID3D11ShaderResourceView> bgraView_;
    ComPtr<ID3D11VideoProcessorOutputView> outputView_;
    ComPtr<ID3D11Texture2D> yTexture_;
    ComPtr<ID3D11ShaderResourceView> yView_;
    ComPtr<ID3D11Texture2D> uvTexture_;
    ComPtr<ID3D11ShaderResourceView> uvView_;
    std::wstring url_;
    bool hardware_ = false;
    int width_ = 0;
    int height_ = 0;
    LONGLONG timestamp_ = 0;
    int hwStage_ = 0;
    long hwCode_ = 0;
};
