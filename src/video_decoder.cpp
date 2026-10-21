#include "video_decoder.h"

#include <algorithm>
#include <cstring>

VideoDecoder::~VideoDecoder()
{
    Close();
}

void VideoDecoder::Close()
{
    outputView_.Reset();
    bgraView_.Reset();
    bgraTexture_.Reset();
    vp_.Reset();
    vpEnumerator_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    yView_.Reset();
    yTexture_.Reset();
    uvView_.Reset();
    uvTexture_.Reset();
    reader_.Reset();
    d3dManager_.Reset();
    context_.Reset();
    device_.Reset();
    url_.clear();
    hardware_ = false;
    width_ = 0;
    height_ = 0;
}

bool VideoDecoder::Open(const std::wstring& path, ID3D11Device* device)
{
    Close();
    if (!device)
        return false;

    device_ = device;
    device_->GetImmediateContext(&context_);
    if (!context_)
        return false;

    url_ = path;
    if (OpenHardware())
        return true;
    return OpenSoftware();
}

bool VideoDecoder::OpenHardware()
{
    reader_.Reset();
    hardware_ = false;
    hwStage_ = 0;
    hwCode_ = 0;

    UINT resetToken = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &d3dManager_);
    if (FAILED(hr))
    {
        hwStage_ = 1;
        hwCode_ = hr;
        return false;
    }

    hr = d3dManager_->ResetDevice(device_.Get(), resetToken);
    if (FAILED(hr))
    {
        hwStage_ = 2;
        hwCode_ = hr;
        return false;
    }

    if (!CreateReaderHardware(url_))
    {
        hwStage_ = 3;
        return false;
    }
    if (!NegotiateOutputType(MFVideoFormat_NV12))
    {
        hwStage_ = 4;
        return false;
    }
    if (!PrepareHardwareResources())
    {
        if (hwStage_ == 0)
            hwStage_ = 5;
        return false;
    }

    hardware_ = true;
    return true;
}

bool VideoDecoder::OpenSoftware()
{
    reader_.Reset();
    outputView_.Reset();
    bgraView_.Reset();
    bgraTexture_.Reset();
    vp_.Reset();
    vpEnumerator_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    d3dManager_.Reset();
    hardware_ = false;
    width_ = 0;
    height_ = 0;

    if (!CreateReaderSoftware(url_))
        return false;
    if (!NegotiateOutputType(MFVideoFormat_NV12))
        return false;
    if (!PrepareSoftwareResources())
        return false;

    return true;
}

bool VideoDecoder::CreateReaderHardware(const std::wstring& url)
{
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 4)))
        return false;

    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, d3dManager_.Get());

    HRESULT hr = MFCreateSourceReaderFromURL(url.c_str(), attributes.Get(), &reader_);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    hr = reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    return true;
}

bool VideoDecoder::CreateReaderSoftware(const std::wstring& url)
{
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 2)))
        return false;

    attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);

    if (FAILED(MFCreateSourceReaderFromURL(url.c_str(), attributes.Get(), &reader_)))
        return false;

    reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE)))
        return false;

    return true;
}

bool VideoDecoder::NegotiateOutputType(const GUID& subtype)
{
    ComPtr<IMFMediaType> nativeType;
    HRESULT hr = reader_->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &nativeType);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    hr = MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (FAILED(hr) || width == 0 || height == 0)
    {
        hwCode_ = hr;
        return false;
    }

    width_ = static_cast<int>(width);
    height_ = static_cast<int>(height);

    ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    UINT32 frameRateNumerator = 0;
    UINT32 frameRateDenominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE,
                                      &frameRateNumerator, &frameRateDenominator)) &&
        frameRateDenominator != 0)
    {
        MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE,
                            frameRateNumerator, frameRateDenominator);
    }

    UINT32 nativeAttribute = 0;
    if (SUCCEEDED(nativeType->GetUINT32(MF_MT_YUV_MATRIX, &nativeAttribute)))
        outputType->SetUINT32(MF_MT_YUV_MATRIX, nativeAttribute);
    if (SUCCEEDED(nativeType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nativeAttribute)))
        outputType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, nativeAttribute);

    hr = reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.Get());
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    return true;
}

bool VideoDecoder::PrepareHardwareResources()
{
    hwStage_ = 7;
    HRESULT hr = device_.As(&videoDevice_);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }
    hr = context_.As(&videoContext_);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate.Numerator = 60;
    content.InputFrameRate.Denominator = 1;
    content.InputWidth = static_cast<UINT>(width_);
    content.InputHeight = static_cast<UINT>(height_);
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = static_cast<UINT>(width_);
    content.OutputHeight = static_cast<UINT>(height_);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hwStage_ = 8;
    hr = videoDevice_->CreateVideoProcessorEnumerator(&content, &vpEnumerator_);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    hwStage_ = 9;
    hr = videoDevice_->CreateVideoProcessor(vpEnumerator_.Get(), 0, &vp_);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    hwStage_ = 5;
    hr = device_->CreateTexture2D(&desc, nullptr, &bgraTexture_);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    hwStage_ = 6;
    hr = device_->CreateShaderResourceView(bgraTexture_.Get(), nullptr, &bgraView_);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    hwStage_ = 10;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputDesc.Texture2D.MipSlice = 0;
    hr = videoDevice_->CreateVideoProcessorOutputView(bgraTexture_.Get(),
                                                      vpEnumerator_.Get(),
                                                      &outputDesc, &outputView_);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    const RECT sourceRect = { 0, 0, width_, height_ };
    const RECT destRect = { 0, 0, width_, height_ };
    videoContext_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &sourceRect);
    videoContext_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &destRect);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor = {};
    inputColor.YCbCr_Matrix = 1;  // BT.709
    inputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &inputColor);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor = {};
    outputColor.RGB_Range = 0;  // Full range RGB.
    outputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    videoContext_->VideoProcessorSetOutputColorSpace(vp_.Get(), &outputColor);

    return true;
}

bool VideoDecoder::PrepareSoftwareResources()
{
    D3D11_TEXTURE2D_DESC lumaDesc = {};
    lumaDesc.Width = static_cast<UINT>(width_);
    lumaDesc.Height = static_cast<UINT>(height_);
    lumaDesc.MipLevels = 1;
    lumaDesc.ArraySize = 1;
    lumaDesc.Format = DXGI_FORMAT_R8_UNORM;
    lumaDesc.SampleDesc.Count = 1;
    lumaDesc.Usage = D3D11_USAGE_DYNAMIC;
    lumaDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    lumaDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device_->CreateTexture2D(&lumaDesc, nullptr, &yTexture_)))
        return false;
    if (FAILED(device_->CreateShaderResourceView(yTexture_.Get(), nullptr, &yView_)))
        return false;

    D3D11_TEXTURE2D_DESC chromaDesc = lumaDesc;
    chromaDesc.Width = static_cast<UINT>((width_ + 1) / 2);
    chromaDesc.Height = static_cast<UINT>((height_ + 1) / 2);
    chromaDesc.Format = DXGI_FORMAT_R8G8_UNORM;

    if (FAILED(device_->CreateTexture2D(&chromaDesc, nullptr, &uvTexture_)))
        return false;
    if (FAILED(device_->CreateShaderResourceView(uvTexture_.Get(), nullptr, &uvView_)))
        return false;

    return true;
}

DecodeResult VideoDecoder::DecodeNext(DecodedFrame& frame)
{
    frame = {};
    if (!reader_)
        return DecodeResult::EndOfStream;

    DWORD flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* sample = nullptr;

    HRESULT hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                                     &flags, &timestamp, &sample);
    if (FAILED(hr))
        return DecodeResult::Retry;

    if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
    {
        if (sample)
            sample->Release();
        return DecodeResult::EndOfStream;
    }

    if (!sample)
        return DecodeResult::Retry;

    if (hardware_)
    {
        bool decoded = DecodeHardware(sample, frame);
        sample->Release();
        if (decoded)
        {
            timestamp_ = timestamp;
            return DecodeResult::Frame;
        }

        // The hardware reader handed back a frame we cannot consume on the
        // GPU. Reopen once in software mode.
        if (OpenSoftware())
            return DecodeResult::Retry;
        return DecodeResult::Retry;
    }

    bool decoded = DecodeSoftware(sample, frame);
    sample->Release();
    if (decoded)
    {
        timestamp_ = timestamp;
        return DecodeResult::Frame;
    }
    return DecodeResult::Retry;
}

bool VideoDecoder::DecodeHardware(IMFSample* sample, DecodedFrame& frame)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)))
        return false;

    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (FAILED(buffer.As(&dxgiBuffer)))
        return false;

    ComPtr<ID3D11Texture2D> sourceTexture;
    if (FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&sourceTexture))))
        return false;

    UINT subresource = 0;
    if (FAILED(dxgiBuffer->GetSubresourceIndex(&subresource)))
        return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
    inputDesc.FourCC = 0;
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice = 0;
    inputDesc.Texture2D.ArraySlice = subresource;

    ComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(sourceTexture.Get(),
                                                              vpEnumerator_.Get(),
                                                              &inputDesc, &inputView);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();

    hr = videoContext_->VideoProcessorBlt(vp_.Get(), outputView_.Get(), 0, 1, &stream);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    frame.srv = bgraView_.Get();
    frame.srvUV = nullptr;
    frame.planarUV = false;
    return true;
}

bool VideoDecoder::DecodeSoftware(IMFSample* sample, DecodedFrame& frame)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)))
        return false;

    const BYTE* data = nullptr;
    int pitch = 0;
    bool locked2d = false;
    bool locked1d = false;
    DWORD bufferLength = 0;
    buffer->GetCurrentLength(&bufferLength);

    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* raw = nullptr;
    LONG stride = 0;

    if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&raw, &stride)))
    {
        if (!raw || stride < width_)
        {
            buffer2d->Unlock2D();
            return false;
        }
        locked2d = true;
        data = raw;
        pitch = static_cast<int>(stride);
    }
    else
    {
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        if (FAILED(buffer->Lock(&raw, &maxLength, &currentLength)) || !raw)
            return false;
        locked1d = true;
        data = raw;
        pitch = width_;
        if (bufferLength == 0)
            bufferLength = currentLength;
    }

    const bool uploaded = UploadSoftwareFrame(data, pitch, bufferLength);

    if (locked2d)
        buffer2d->Unlock2D();
    if (locked1d)
        buffer->Unlock();

    if (!uploaded)
        return false;

    frame.srv = yView_.Get();
    frame.srvUV = uvView_.Get();
    frame.planarUV = true;
    return true;
}

bool VideoDecoder::UploadSoftwareFrame(const BYTE* data, int pitch, DWORD bufferLength)
{
    if (!data || pitch < width_)
        return false;

    const int chromaWidth = (width_ + 1) / 2;
    const int chromaHeight = (height_ + 1) / 2;

    int planeHeight = height_;
    if (bufferLength > 0)
    {
        const size_t estimate =
            static_cast<size_t>(bufferLength) * 2 / (3 * static_cast<size_t>(pitch));
        if (estimate >= static_cast<size_t>(height_) &&
            estimate <= static_cast<size_t>(height_) + 64)
        {
            planeHeight = static_cast<int>(estimate);
        }
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(yTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;

    for (int y = 0; y < height_; ++y)
    {
        std::memcpy(static_cast<BYTE*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * y,
                    data + static_cast<size_t>(pitch) * y,
                    static_cast<size_t>(width_));
    }
    context_->Unmap(yTexture_.Get(), 0);

    const BYTE* chromaPlane = data + static_cast<size_t>(pitch) * static_cast<size_t>(planeHeight);
    if (FAILED(context_->Map(uvTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;

    for (int y = 0; y < chromaHeight; ++y)
    {
        std::memcpy(static_cast<BYTE*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * y,
                    chromaPlane + static_cast<size_t>(pitch) * y,
                    static_cast<size_t>(chromaWidth) * 2);
    }
    context_->Unmap(uvTexture_.Get(), 0);

    return true;
}

bool VideoDecoder::Rewind()
{
    if (!reader_)
        return false;

    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = 0;

    const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    return SUCCEEDED(hr);
}
