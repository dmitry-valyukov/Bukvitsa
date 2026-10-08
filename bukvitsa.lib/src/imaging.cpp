#include "bukvitsa/reader/imaging.h"

#include <wincodec.h>

namespace bukvitsa::reader {

using Microsoft::WRL::ComPtr;

ComPtr<IWICFormatConverter> decodeImage(std::string_view bytes) {
    if (bytes.empty()) return nullptr;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic))))
        return nullptr;

    // Поток поверх памяти вызывающего, без копии: WIC обещает в неё не писать,
    // а неконстантный указатель просит по сигнатуре.
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return nullptr;
    if (FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
            static_cast<DWORD>(bytes.size()))))
        return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                            &decoder)))
        return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))) return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)))
        return nullptr;

    return converter;
}

}  // namespace bukvitsa::reader
