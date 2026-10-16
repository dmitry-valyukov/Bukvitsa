#include <algorithm>
#include <cmath>
#include <limits>

#include "bukvitsa/reader/imaging.h"

#include <wincodec.h>

#include "bukvitsa/typography/system_failure.h"

// Последним: остановка на нарушенном контракте — wxl.core.
import wxl.core;

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

    // Два рода отказов. Своё — поток, конвертер, уменьшение — не создаётся и
    // не настраивается лишь от беды системы: остановка. Всё, что разбирает сами
    // байты, — ответ о файле: декодер на битых байтах отвечает и WINCODEC_ERR_*,
    // и E_FAIL, различить их нельзя, и все они про файл — «не картинка».
    // Огромный заявленный размер сюда не доходит: его отсекает бюджет памяти
    // по заголовку (frameHeaderOf).
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

/// Заголовок кадра: размер и, по формату пикселей, укладывается ли он в
/// бюджет памяти (withinPictureBudget). Нулевая сторона, формат, о котором у
/// WIC нет сведений, — пусто, «не картинка»: всё это из чужих байт.
std::optional<PictureHeader> frameHeaderOf(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame) {
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return std::nullopt;

    WICPixelFormatGUID format{};
    if (FAILED(frame->GetPixelFormat(&format))) return std::nullopt;
    ComPtr<IWICComponentInfo> component;
    if (FAILED(wic->CreateComponentInfo(format, &component))) return std::nullopt;
    ComPtr<IWICPixelFormatInfo> formatInfo;
    if (FAILED(component.As(&formatInfo))) return std::nullopt;
    UINT bitsPerPixel = 0;
    if (FAILED(formatInfo->GetBitsPerPixel(&bitsPerPixel))) return std::nullopt;

    return PictureHeader{PictureSize{width, height}, !withinPictureBudget(width, height, bitsPerPixel)};
}

}  // namespace

DecodedPicture decodeImage(std::string_view bytes, uint32_t maxSide) {
    if (bytes.empty()) return DecodedPicture{nullptr, PictureVerdict{PictureRefusal::NotAnImage, {}}};

    return decodeImage(newImagingFactory().Get(), bytes, maxSide);
}

PictureSize fittedSize(uint32_t width, uint32_t height, uint32_t limit) {
    if (limit == 0 || width == 0 || height == 0) wxl::core::abort("fittedSize: нулевая сторона или предел");
    if (width <= limit && height <= limit) return PictureSize{width, height};

    // Большая сторона — ровно предел, меньшая — пропорционально, целочисленно
    // в uint64_t (произведение двух сторон в uint32_t переполнилось бы), не
    // меньше пикселя.
    const bool wide = width >= height;
    const uint64_t larger = wide ? width : height;
    const uint64_t smaller = wide ? height : width;
    const auto fittedSmaller = static_cast<uint32_t>(std::max<uint64_t>(1, smaller * limit / larger));
    return wide ? PictureSize{limit, fittedSmaller} : PictureSize{fittedSmaller, limit};
}

std::optional<PictureHeader> pictureSizeOf(IWICImagingFactory* wic, std::string_view bytes) {
    const ComPtr<IWICBitmapFrameDecode> frame = firstFrameOf(wic, bytes);
    if (!frame) return std::nullopt;

    return frameHeaderOf(wic, frame.Get());
}

DecodedPicture decodeImage(IWICImagingFactory* wic, std::string_view bytes, uint32_t maxSide) {
    const auto notAnImage = DecodedPicture{nullptr, PictureVerdict{PictureRefusal::NotAnImage, {}}};

    const ComPtr<IWICBitmapFrameDecode> frame = firstFrameOf(wic, bytes);
    if (!frame) return notAnImage;

    const std::optional<PictureHeader> header = frameHeaderOf(wic, frame.Get());
    if (!header) return notAnImage;
    if (header->tooLarge) return DecodedPicture{nullptr, PictureVerdict{PictureRefusal::TooLarge, header->size}};

    const PictureSize fitted = fittedSize(header->size.width, header->size.height, maxSide);
    const bool tooWide = fitted.width != header->size.width || fitted.height != header->size.height;

    // Уменьшение прямо на кадре — только там, где оно что-то даёт: у кадра с
    // IWICBitmapSourceTransform (JPEG — DCT в 1/2, 1/4, 1/8) встроенное
    // уменьшение WIC не раскодирует огромный снимок целиком, а за конвертером
    // эта возможность теряется. Его отказ на кадре — ответ о формате файла:
    // тогда общий путь. Общий путь — для всех прочих, индексных с прозрачностью
    // (GIF, PNG-8 с tRNS) в том числе: конвертер в предумноженный PBGRA, затем
    // уменьшение — уменьшать до предумножения значит ореолы, — и там формат
    // известен, так что отказ уменьшения — своё, остановка.
    ComPtr<IWICBitmapSource> pixelSource = frame;
    bool shrunkOnFrame = false;
    if (ComPtr<IWICBitmapSourceTransform> transform; tooWide && SUCCEEDED(frame.As(&transform))) {
        ComPtr<IWICBitmapScaler> frameScaler;
        stopIfFailed(wic->CreateBitmapScaler(&frameScaler), "WIC: не создалось уменьшение снимка");
        if (SUCCEEDED(frameScaler->Initialize(frame.Get(), fitted.width, fitted.height,
                                              WICBitmapInterpolationModeFant))) {
            pixelSource = frameScaler;
            shrunkOnFrame = true;
        }
    }

    // Конвертер — своё: остановка; его настройка на кадр — разбор чужих байт.
    ComPtr<IWICFormatConverter> converter;
    stopIfFailed(wic->CreateFormatConverter(&converter), "WIC: не создался конвертер");
    if (FAILED(converter->Initialize(pixelSource.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        return notAnImage;
    pixelSource = converter;

    if (tooWide && !shrunkOnFrame) {
        ComPtr<IWICBitmapScaler> scaler;
        stopIfFailed(wic->CreateBitmapScaler(&scaler), "WIC: не создалось уменьшение снимка");
        stopIfFailed(scaler->Initialize(pixelSource.Get(), fitted.width, fitted.height,
                                        WICBitmapInterpolationModeFant),
                     "WIC: уменьшение снимка не настроилось");
        pixelSource = scaler;
    }

    // Пиксели — здесь, целиком (WICBitmapCacheOnLoad): битые данные
    // отказывают сейчас, словом о файле, а не потом, на каждом кадре. Нехватка
    // памяти здесь — остановка. Бюджет ограничил только выход кадра в его
    // формате, и обычно нехватка — беда системы; но прогрессивный JPEG держит
    // по ходу ещё коэффициенты (4:4:4 — 6 байт на пиксель, CMYK — 8, в
    // пределах бюджета до 2 ГиБ), и тогда это остановка от данных — известная
    // цена бюджета, который пропускает снимки телефонов на 200 Мп. Прочие
    // отказы — о файле.
    ComPtr<IWICBitmap> decodedPicture;
    const HRESULT cacheResult = wic->CreateBitmapFromSource(pixelSource.Get(), WICBitmapCacheOnLoad, &decodedPicture);
    if (cacheResult == E_OUTOFMEMORY) stopIfFailed(cacheResult, "WIC: не хватило памяти на снимок в бюджете");
    if (FAILED(cacheResult)) return notAnImage;

    return DecodedPicture{decodedPicture, PictureVerdict{}};
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
