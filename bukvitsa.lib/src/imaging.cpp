#include <algorithm>
#include <cmath>
#include <limits>

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

namespace {

/// Первый кадр картинки — заголовок разобран, пикселей ещё нет; nullptr —
/// байты не картинка.
ComPtr<IWICBitmapFrameDecode> firstFrameOf(IWICImagingFactory* wic, std::string_view bytes) {
    if (bytes.empty()) return nullptr;

    // Длина — из файла (подложка и проба мастера читают его целиком), а поток
    // WIC меряет память в DWORD: больше — не картинка, как прочие чужие
    // данные, а не длина, молча обрезанная приведением.
    if (bytes.size() > std::numeric_limits<DWORD>::max()) return nullptr;

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

    return frame;
}

/// Размер, уменьшенный до предела по большей стороне с сохранением пропорций;
/// сторона — не меньше пикселя.
PictureSize fittedSize(UINT width, UINT height, uint32_t limit) {
    const double fitShare = static_cast<double>(limit) / static_cast<double>(std::max(width, height));
    const auto fitSide = [&](UINT side) {
        return std::clamp(static_cast<uint32_t>(std::floor(side * fitShare)), uint32_t{1}, limit);
    };
    return PictureSize{fitSide(width), fitSide(height)};
}

}  // namespace

ComPtr<IWICBitmap> decodeImage(std::string_view bytes, uint32_t maxSide) {
    if (bytes.empty()) return nullptr;

    return decodeImage(newImagingFactory().Get(), bytes, maxSide);
}

std::optional<PictureSize> pictureSizeOf(IWICImagingFactory* wic, std::string_view bytes) {
    const ComPtr<IWICBitmapFrameDecode> frame = firstFrameOf(wic, bytes);
    if (!frame) return std::nullopt;

    // Размер — из заголовка, то есть тоже из чужих байт; нулевой стороны у
    // картинки не бывает.
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return std::nullopt;

    return PictureSize{width, height};
}

ComPtr<IWICBitmap> decodeImage(IWICImagingFactory* wic, std::string_view bytes, uint32_t maxSide) {
    const ComPtr<IWICBitmapFrameDecode> frame = firstFrameOf(wic, bytes);
    if (!frame) return nullptr;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return nullptr;

    // Конвертер и уменьшение — своё: не создались — остановка; их настройка
    // на кадр и пиксели — разбор чужих байт. Уменьшение — за конвертером: в
    // 32bppPBGRA Fant работает при любом формате файла. Цепочка ленивая, и
    // память под пиксели выделяет только CacheOnLoad в конце — уже под
    // уменьшенный размер, а не под заявленный в заголовке.
    ComPtr<IWICFormatConverter> converter;
    stopIfFailed(wic->CreateFormatConverter(&converter), "WIC: не создался конвертер");
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeMedianCut)))
        return nullptr;

    ComPtr<IWICBitmapSource> fittedSource = converter;
    if (width > maxSide || height > maxSide) {
        const PictureSize fitted = fittedSize(width, height, maxSide);
        ComPtr<IWICBitmapScaler> scaler;
        stopIfFailed(wic->CreateBitmapScaler(&scaler), "WIC: не создалось уменьшение снимка");
        if (FAILED(scaler->Initialize(converter.Get(), fitted.width, fitted.height, WICBitmapInterpolationModeFant)))
            return nullptr;
        fittedSource = scaler;
    }

    // Пиксели — здесь, целиком (WICBitmapCacheOnLoad): битые данные
    // отказывают сейчас, словом о файле, а не потом, на каждом кадре.
    ComPtr<IWICBitmap> decodedPicture;
    if (FAILED(wic->CreateBitmapFromSource(fittedSource.Get(), WICBitmapCacheOnLoad, &decodedPicture)))
        return nullptr;

    return decodedPicture;
}

ComPtr<ID2D1Bitmap1> deviceBitmapOf(ID2D1DeviceContext* context, IWICBitmapSource* picture) {
    UINT width = 0;
    UINT height = 0;
    stopIfFailed(picture->GetSize(&width, &height), "WIC: снимок не назвал размер");

    // Предел устройства известен заранее (на Windows 10 бывают устройства
    // уровня 9.x с пределом 2048–4096), и больший битмап Direct2D отверг бы —
    // это не сбой, а мера. Снимок от decodeImage уже не больше kMaxPictureSide;
    // здесь он уменьшается, только если предел устройства ниже.
    ComPtr<IWICBitmapSource> fittedPicture = picture;
    const UINT32 deviceLimit = context->GetMaximumBitmapSize();
    if (width > deviceLimit || height > deviceLimit) {
        const PictureSize fitted = fittedSize(width, height, deviceLimit);
        ComPtr<IWICBitmapScaler> scaler;
        stopIfFailed(newImagingFactory()->CreateBitmapScaler(&scaler), "WIC: не создалось уменьшение снимка");
        stopIfFailed(scaler->Initialize(picture, fitted.width, fitted.height, WICBitmapInterpolationModeFant),
                     "WIC: снимок не уменьшился до предела устройства");
        fittedPicture = scaler;
    }

    ComPtr<ID2D1Bitmap1> bitmap;
    stopIfFailed(context->CreateBitmapFromWicBitmap(fittedPicture.Get(), nullptr, &bitmap),
                 "Direct2D: снимок не стал битмапом устройства");
    return bitmap;
}

}  // namespace bukvitsa::reader
