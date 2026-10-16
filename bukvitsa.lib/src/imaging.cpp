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
    // Огромный заявленный размер сюда не доходит: его отсекает бюджет пикселей
    // по заголовку (frameSizeOf).
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

/// Размер кадра из заголовка, если он годится для раскодирования: стороны не
/// нулевые и пиксели в бюджете (withinPictureBudget); иначе — пусто, «не
/// картинка», до всякого раскодирования.
std::optional<PictureSize> frameSizeOf(IWICBitmapFrameDecode* frame) {
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || !withinPictureBudget(width, height)) return std::nullopt;
    return PictureSize{width, height};
}

/// Есть ли у пикселей кадра прозрачность — по формату кадра. Формат — из
/// чужих байт: незнакомый WIC — пусто, «не картинка»; сведения о знакомом —
/// своё, отказ — остановка.
std::optional<bool> frameHasAlpha(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame) {
    WICPixelFormatGUID format{};
    if (FAILED(frame->GetPixelFormat(&format))) return std::nullopt;

    ComPtr<IWICComponentInfo> component;
    if (FAILED(wic->CreateComponentInfo(format, &component))) return std::nullopt;

    ComPtr<IWICPixelFormatInfo2> formatInfo;
    stopIfFailed(component.As(&formatInfo), "WIC: формат пикселей без сведений о прозрачности");
    BOOL transparent = FALSE;
    stopIfFailed(formatInfo->SupportsTransparency(&transparent), "WIC: формат пикселей не ответил о прозрачности");
    return transparent != FALSE;
}

}  // namespace

ComPtr<IWICBitmap> decodeImage(std::string_view bytes, uint32_t maxSide) {
    if (bytes.empty()) return nullptr;

    return decodeImage(newImagingFactory().Get(), bytes, maxSide);
}

PictureSize fittedSize(uint32_t width, uint32_t height, uint32_t limit) {
    if (limit == 0) wxl::core::abort("fittedSize: предел в ноль пикселей");

    // Большая сторона — ровно предел, меньшая — пропорционально, целочисленно
    // в uint64_t (произведение двух сторон в uint32_t переполнилось бы), не
    // меньше пикселя.
    const bool wide = width >= height;
    const uint64_t larger = wide ? width : height;
    const uint64_t smaller = wide ? height : width;
    const auto fittedSmaller = static_cast<uint32_t>(std::max<uint64_t>(1, smaller * limit / larger));
    return wide ? PictureSize{limit, fittedSmaller} : PictureSize{fittedSmaller, limit};
}

std::optional<PictureSize> pictureSizeOf(IWICImagingFactory* wic, std::string_view bytes) {
    const ComPtr<IWICBitmapFrameDecode> frame = firstFrameOf(wic, bytes);
    if (!frame) return std::nullopt;

    // Размер — из заголовка, то есть тоже из чужих байт.
    return frameSizeOf(frame.Get());
}

ComPtr<IWICBitmap> decodeImage(IWICImagingFactory* wic, std::string_view bytes, uint32_t maxSide) {
    const ComPtr<IWICBitmapFrameDecode> frame = firstFrameOf(wic, bytes);
    if (!frame) return nullptr;

    const std::optional<PictureSize> frameSize = frameSizeOf(frame.Get());
    if (!frameSize) return nullptr;

    const std::optional<bool> hasAlpha = frameHasAlpha(wic, frame.Get());
    if (!hasAlpha) return nullptr;

    // Уменьшение (Fant) — своё: на известном формате и размере оно чужих
    // байтов не разбирает, и отказ его создания и настройки — остановка.
    const bool tooLarge = frameSize->width > maxSide || frameSize->height > maxSide;
    const auto shrinkSource = [&](IWICBitmapSource* source) {
        const PictureSize fitted = fittedSize(frameSize->width, frameSize->height, maxSide);
        ComPtr<IWICBitmapScaler> scaler;
        stopIfFailed(wic->CreateBitmapScaler(&scaler), "WIC: не создалось уменьшение снимка");
        stopIfFailed(scaler->Initialize(source, fitted.width, fitted.height, WICBitmapInterpolationModeFant),
                     "WIC: уменьшение снимка не настроилось");
        return ComPtr<IWICBitmapSource>{scaler};
    };

    // Где стоит уменьшение — зависит от прозрачности. Без неё — прямо за
    // кадром: так встроенное уменьшение WIC пользуется IWICBitmapSourceTransform
    // кадра (у JPEG — DCT в 1/2, 1/4, 1/8), и декодер не раскодирует огромный
    // снимок целиком; за конвертером эта возможность теряется. С альфой —
    // сперва конвертер в предумноженный PBGRA, потом уменьшение: уменьшать до
    // предумножения — ореолы по краям прозрачного.
    ComPtr<IWICBitmapSource> pixelSource = frame;
    if (tooLarge && !*hasAlpha) pixelSource = shrinkSource(pixelSource.Get());

    // Конвертер — своё: остановка; его настройка на кадр — разбор чужих байт.
    ComPtr<IWICFormatConverter> converter;
    stopIfFailed(wic->CreateFormatConverter(&converter), "WIC: не создался конвертер");
    if (FAILED(converter->Initialize(pixelSource.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeMedianCut)))
        return nullptr;
    pixelSource = converter;

    if (tooLarge && *hasAlpha) pixelSource = shrinkSource(pixelSource.Get());

    // Пиксели — здесь, целиком (WICBitmapCacheOnLoad): битые данные
    // отказывают сейчас, словом о файле, а не потом, на каждом кадре. Битмап
    // памяти — под уменьшенный размер; сколько держит сам декодер по ходу
    // (Adam7, прогрессивный JPEG — бывает, что полный буфер), ограничивает
    // только бюджет пикселей. Нехватка памяти после бюджета — беда системы:
    // остановка; прочие отказы — о файле.
    ComPtr<IWICBitmap> decodedPicture;
    const HRESULT cacheResult = wic->CreateBitmapFromSource(pixelSource.Get(), WICBitmapCacheOnLoad, &decodedPicture);
    if (cacheResult == E_OUTOFMEMORY) stopIfFailed(cacheResult, "WIC: не хватило памяти на пиксели снимка в бюджете");
    if (FAILED(cacheResult)) return nullptr;

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
