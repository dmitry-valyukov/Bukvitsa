#include <algorithm>
#include <cmath>

#include "bukvitsa/reader/imaging.h"

#include <wincodec.h>

#include "bukvitsa/typography/system_failure.h"

namespace bukvitsa::reader {

using Microsoft::WRL::ComPtr;
using typography::stopIfFailed;

namespace {

/// Фабрика WIC — системная: не создалась — остановка, а не «не картинка».
ComPtr<IWICImagingFactory> newImagingFactory() {
    ComPtr<IWICImagingFactory> wic;
    stopIfFailed(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
                 "WIC: не создалась фабрика");
    return wic;
}

}  // namespace

ComPtr<IWICBitmap> decodeImage(std::string_view bytes) {
    if (bytes.empty()) return nullptr;

    return decodeImage(newImagingFactory().Get(), bytes);
}

ComPtr<IWICBitmap> decodeImage(IWICImagingFactory* wic, std::string_view bytes) {
    if (bytes.empty()) return nullptr;

    // Два рода отказов. Своё — поток и конвертер — не создаётся лишь от беды
    // системы: остановка. Всё, что разбирает сами байты, — ответ о файле:
    // декодер на битых байтах отвечает и WINCODEC_ERR_*, и E_FAIL, и
    // E_OUTOFMEMORY на огромном заявленном размере, различить их нельзя, и все
    // они про файл — «не картинка».
    //
    // Поток поверх памяти вызывающего, без копии: WIC обещает в неё не писать,
    // а неконстантный указатель просит по сигнатуре; нужна она только до
    // конца вызова.
    ComPtr<IWICStream> stream;
    stopIfFailed(wic->CreateStream(&stream), "WIC: не создался поток");
    if (FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
                                            static_cast<DWORD>(bytes.size()))))
        return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)))
        return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

    ComPtr<IWICFormatConverter> converter;
    stopIfFailed(wic->CreateFormatConverter(&converter), "WIC: не создался конвертер");
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeMedianCut)))
        return nullptr;

    // Пиксели — здесь, целиком (WICBitmapCacheOnLoad): битые данные
    // отказывают сейчас, словом о файле, а не потом, на каждом кадре.
    ComPtr<IWICBitmap> decodedPicture;
    if (FAILED(wic->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &decodedPicture)))
        return nullptr;

    return decodedPicture;
}

ComPtr<ID2D1Bitmap1> deviceBitmapOf(ID2D1DeviceContext* context, IWICBitmapSource* picture) {
    UINT width = 0;
    UINT height = 0;
    stopIfFailed(picture->GetSize(&width, &height), "WIC: снимок не назвал размер");

    // Предел устройства известен заранее (на Windows 10 бывают устройства
    // уровня 9.x с пределом 2048–4096), и больший битмап Direct2D отверг бы —
    // это не сбой, а мера. Снимок уменьшается до неё.
    ComPtr<IWICBitmapSource> fittedPicture = picture;
    const UINT32 deviceLimit = context->GetMaximumBitmapSize();
    if (width > deviceLimit || height > deviceLimit) {
        const double fitShare = static_cast<double>(deviceLimit) / static_cast<double>(std::max(width, height));
        const auto fitSide = [&](UINT side) {
            return std::clamp(static_cast<UINT>(std::floor(side * fitShare)), UINT{1}, UINT{deviceLimit});
        };

        ComPtr<IWICBitmapScaler> scaler;
        stopIfFailed(newImagingFactory()->CreateBitmapScaler(&scaler), "WIC: не создалось уменьшение снимка");
        stopIfFailed(scaler->Initialize(picture, fitSide(width), fitSide(height), WICBitmapInterpolationModeFant),
                     "WIC: снимок не уменьшился до предела устройства");
        fittedPicture = scaler;
    }

    ComPtr<ID2D1Bitmap1> bitmap;
    stopIfFailed(context->CreateBitmapFromWicBitmap(fittedPicture.Get(), nullptr, &bitmap),
                 "Direct2D: снимок не стал битмапом устройства");
    return bitmap;
}

}  // namespace bukvitsa::reader
