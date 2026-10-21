#include "video_decoder.h"

#include <algorithm>
#include <cstring>

VideoDecoder::~VideoDecoder()
{
    Close();
}

void VideoDecoder::Close()
{
    ReleaseOutputTarget();
    currentInput_ = nullptr;
    inputViews_.clear();
    inputTexture_.Reset();
    currentSample_.Reset();
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
    ReleaseOutputTarget();
    currentInput_ = nullptr;
    inputViews_.clear();
    inputTexture_.Reset();
    currentSample_.Reset();
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
    ComPtr<IMFMediaType> outputType;
    HRESULT hr = MFCreateMediaType(&outputType);
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    // Only the subtype: any other attribute that differs from the decoder's
    // native output makes the reader insert its own video processor.
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, subtype);

    hr = reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outputType.Get());
    if (FAILED(hr))
    {
        hwCode_ = hr;
        return false;
    }

    return ReadOutputType();
}

bool VideoDecoder::ReadOutputType()
{
    ComPtr<IMFMediaType> type;
    HRESULT hr = reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
    UINT32 codedWidth = 0;
    UINT32 codedHeight = 0;
    if (SUCCEEDED(hr))
        hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &codedWidth, &codedHeight);
    if (FAILED(hr) || codedWidth == 0 || codedHeight == 0)
    {
        hwCode_ = hr;
        return false;
    }

    MFVideoArea area = {};
    UINT8* blob = reinterpret_cast<UINT8*>(&area);
    const UINT32 blobSize = static_cast<UINT32>(sizeof(area));
    if (FAILED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, blob, blobSize, nullptr)))
        type->GetBlob(MF_MT_GEOMETRIC_APERTURE, blob, blobSize, nullptr);

    codedWidth_ = static_cast<int>(codedWidth);
    codedHeight_ = static_cast<int>(codedHeight);
    apertureX_ = area.OffsetX.value;
    apertureY_ = area.OffsetY.value;
    width_ = static_cast<int>(area.Area.cx);
    height_ = static_cast<int>(area.Area.cy);
    if (apertureX_ < 0 || apertureY_ < 0 || width_ <= 0 || height_ <= 0 ||
        width_ > codedWidth_ - apertureX_ || height_ > codedHeight_ - apertureY_)
    {
        apertureX_ = 0;
        apertureY_ = 0;
        width_ = codedWidth_;
        height_ = codedHeight_;
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
    content.InputWidth = static_cast<UINT>(codedWidth_);
    content.InputHeight = static_cast<UINT>(codedHeight_);
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

    // No driver "enhancement" passes (denoise, edge enhancement, ...).
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);

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
    currentInput_ = nullptr;
    if (!reader_)
        return DecodeResult::EndOfStream;

    DWORD flags = 0;
    LONGLONG timestamp = 0;

    // Replaces (releases) the previous frame, whose blit is already submitted.
    HRESULT hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                                     &flags, &timestamp, &currentSample_);
    if (FAILED(hr))
        return DecodeResult::Retry;

    if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        return DecodeResult::EndOfStream;

    if (!currentSample_)
        return DecodeResult::Retry;

    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
    {
        if (!ReadOutputType())
            return DecodeResult::Retry;
        if (hardware_)
        {
            // The video processor and its views are sized for the old type.
            inputViews_.clear();
            inputTexture_.Reset();
            ReleaseOutputTarget();
            if (!PrepareHardwareResources())
            {
                OpenSoftware();
                return DecodeResult::Retry;
            }
        }
        else if (!PrepareSoftwareResources())
        {
            return DecodeResult::Retry;
        }
    }

    if (hardware_)
    {
        if (DecodeHardware(currentSample_.Get(), frame))
        {
            timestamp_ = timestamp;
            return DecodeResult::Frame;
        }

        // The hardware reader handed back a frame we cannot consume on the
        // GPU. Reopen once in software mode.
        OpenSoftware();
        return DecodeResult::Retry;
    }

    if (DecodeSoftware(currentSample_.Get(), frame))
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

    // The decoder cycles through the slices of one texture array, so the
    // input views are created once per slice.
    if (sourceTexture.Get() != inputTexture_.Get())
    {
        inputViews_.clear();
        inputTexture_ = sourceTexture;
    }
    if (subresource >= inputViews_.size())
        inputViews_.resize(subresource + 1);

    ComPtr<ID3D11VideoProcessorInputView>& inputView = inputViews_[subresource];
    if (!inputView)
    {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;
        inputDesc.Texture2D.ArraySlice = subresource;

        const HRESULT hr = videoDevice_->CreateVideoProcessorInputView(sourceTexture.Get(),
                                                                        vpEnumerator_.Get(),
                                                                        &inputDesc, &inputView);
        if (FAILED(hr))
        {
            hwCode_ = hr;
            return false;
        }
    }

    currentInput_ = inputView.Get();
    frame.hardware = true;
    return true;
}

HRESULT VideoDecoder::BlitTo(ID3D11Texture2D* target, int targetWidth, int targetHeight)
{
    if (!hardware_ || !currentInput_ || !target || targetWidth <= 0 || targetHeight <= 0)
        return E_FAIL;

    HRESULT hr = S_OK;
    if (target != outputTarget_)
    {
        ReleaseOutputTarget();
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputDesc.Texture2D.MipSlice = 0;
        hr = videoDevice_->CreateVideoProcessorOutputView(target, vpEnumerator_.Get(),
                                                          &outputDesc, &outputView_);
        if (SUCCEEDED(hr))
            outputTarget_ = target;
    }

    if (SUCCEEDED(hr))
    {
        // Cover: fill the target and center-crop the excess of the visible region.
        const double scale = (std::max)(static_cast<double>(targetWidth) / width_,
                                        static_cast<double>(targetHeight) / height_);
        const LONG cropWidth = static_cast<LONG>(targetWidth / scale + 0.5);
        const LONG cropHeight = static_cast<LONG>(targetHeight / scale + 0.5);
        const LONG left = apertureX_ + (width_ - cropWidth) / 2;
        const LONG top = apertureY_ + (height_ - cropHeight) / 2;
        const RECT sourceRect = { left, top, left + cropWidth, top + cropHeight };
        const RECT destRect = { 0, 0, targetWidth, targetHeight };
        videoContext_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &sourceRect);
        videoContext_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &destRect);

        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = currentInput_;
        hr = videoContext_->VideoProcessorBlt(vp_.Get(), outputView_.Get(), 0, 1, &stream);
    }

    if (FAILED(hr))
    {
        // The GPU cannot process this frame. Reopen once in software mode.
        hwCode_ = hr;
        OpenSoftware();
    }
    return hr;
}

void VideoDecoder::ReleaseOutputTarget()
{
    outputView_.Reset();
    outputTarget_ = nullptr;
}

bool VideoDecoder::DecodeSoftware(IMFSample* sample, DecodedFrame& frame)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)))
        return false;

    BYTE* data = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    bool uploaded = false;

    // Lock2DSize reports the real locked length; GetCurrentLength would be
    // the contiguous (unpadded) size.
    ComPtr<IMF2DBuffer2> buffer2d;
    BYTE* bufferStart = nullptr;
    if (SUCCEEDED(buffer.As(&buffer2d)) &&
        SUCCEEDED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &data, &pitch, &bufferStart, &length)))
    {
        uploaded = UploadSoftwareFrame(data, pitch, length - static_cast<DWORD>(data - bufferStart));
        buffer2d->Unlock2D();
    }
    else
    {
        DWORD maxLength = 0;
        if (FAILED(buffer->Lock(&data, &maxLength, &length)))
            return false;
        uploaded = UploadSoftwareFrame(data, codedWidth_, length);
        buffer->Unlock();
    }

    if (!uploaded)
        return false;

    frame.srv = yView_.Get();
    frame.srvUV = uvView_.Get();
    return true;
}

bool VideoDecoder::UploadSoftwareFrame(const BYTE* data, LONG pitch, DWORD bufferLength)
{
    if (!data || pitch < codedWidth_)
        return false;

    const size_t stride = static_cast<size_t>(pitch);
    const int chromaWidth = (width_ + 1) / 2;
    const int chromaHeight = (height_ + 1) / 2;

    // NV12: the chroma plane follows codedHeight luma rows. Copy only the
    // visible region, and never read past the end of the buffer.
    const BYTE* luma = data + stride * apertureY_ + apertureX_;
    const size_t chromaOffset = stride * (codedHeight_ + apertureY_ / 2) + (apertureX_ & ~1);
    const size_t chromaEnd = chromaOffset + stride * (chromaHeight - 1) +
                             static_cast<size_t>(chromaWidth) * 2;
    if (chromaEnd > bufferLength)
        return false;
    const BYTE* chroma = data + chromaOffset;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(yTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;

    for (int y = 0; y < height_; ++y)
    {
        std::memcpy(static_cast<BYTE*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * y,
                    luma + stride * y,
                    static_cast<size_t>(width_));
    }
    context_->Unmap(yTexture_.Get(), 0);

    if (FAILED(context_->Map(uvTexture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;

    for (int y = 0; y < chromaHeight; ++y)
    {
        std::memcpy(static_cast<BYTE*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * y,
                    chroma + stride * y,
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
