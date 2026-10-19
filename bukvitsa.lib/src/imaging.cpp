#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>

#include <wincodec.h>

#include "bukvitsa/reader/imaging.h"

#include "bukvitsa/typography/system_failure.h"

// Последним: раскодирование в пуле — операция wxl.async, остановка на
// нарушенном контракте — wxl.core.
import wxl.async;
import wxl.core;

namespace bukvitsa::reader {

using Microsoft::WRL::ComPtr;
using typography::stopIfFailed;
using typography::stopIfFailedOffDevice;
using wxl::async::cancellation_token;
using wxl::async::operation_canceled_exception;
using wxl::async::orphan_state;
using wxl::async::orphanable;
using wxl::async::sta_loop;
using wxl::async::task;

namespace {

/// Фабрика WIC — системная: не создалась — остановка, а не «не картинка».
/// Отказ — без проверки на потерю устройства: фабрику заводят и в пуле, а у
/// WIC устройства нет. Поток пула COM сам не поднимает и живёт в неявной MTA
/// процесса — её держит рабочий поток петли wxl; нет её — CoCreateInstance
/// отвечает CO_E_NOTINITIALIZED, и это тоже остановка: ошибка в коде, а не
/// ответ о файле.
ComPtr<IWICImagingFactory> newImagingFactory() {
    ComPtr<IWICImagingFactory> wic;
    stopIfFailedOffDevice(
        ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
        "WIC: не создалась фабрика");
    return wic;
}

/// Строк в полосе копирования кадра: у снимка в предел 4096 по стороне —
/// 4 МиБ PBGRA на полосу и шестнадцать полос на снимок; осиротевшее тело
/// дорабатывает одну полосу, а не весь снимок, и вызовов WIC немного.
constexpr uint32_t kBandRows = 256;

/// Ответ «не картинка»: чужие байты, а не отказ системы.
DecodedPicture notAnImage() {
    return DecodedPicture{PictureVerdict{PictureRefusal::NotAnImage, {}}, {}};
}

/// Вопрос раскодирования в пуле между его ступенями: тело, которое бросили —
/// ждущий отменил ожидание или ушёл сам, — осиротело (`stop_requested`) и
/// кончается отменой, а не идёт к следующей ступени: его ответа уже никто не
/// возьмёт. Вызов WIC посреди не обрывается, так что остановить тело можно
/// только здесь. На потоке вызова (`state` пусто) спрашивать некого.
void throwIfStopRequested(const orphan_state* state) {
    if (state && state->stop_requested()) throw operation_canceled_exception();
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
    stopIfFailedOffDevice(wic->CreateStream(&stream), "WIC: не создался поток");
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

/// Раскодирование — общее у формы на потоке вызова и формы в пуле; в пуле
/// `state` — его состояние сироты, и между ступенями раскодирования оно
/// спрашивается (throwIfStopRequested), а `afterBand` — ступень проб, после
/// каждой полосы.
/// Отказы системы — остановка без проверки на потерю устройства: у WIC его
/// нет, а в пуле проверку, код потока окна, звать нельзя.
DecodedPicture decodeWith(IWICImagingFactory* wic, std::string_view bytes, uint32_t maxSide,
                          const orphan_state* state, const std::function<void(uint32_t)>& afterBand) {
    // Предел — не больше kMaxPictureSide: тогда пиксели — до 64 МБ, и полоса
    // в байтах укладывается в UINT, которым меряет WIC.
    if (maxSide == 0 || maxSide > kMaxPictureSide) wxl::core::abort("decodeImage: предел вне (0, kMaxPictureSide]");

    throwIfStopRequested(state);
    const ComPtr<IWICBitmapFrameDecode> frame = firstFrameOf(wic, bytes);
    if (!frame) return notAnImage();

    throwIfStopRequested(state);
    const std::optional<PictureHeader> header = frameHeaderOf(wic, frame.Get());
    if (!header) return notAnImage();
    if (header->tooLarge) return DecodedPicture{PictureVerdict{PictureRefusal::TooLarge, header->size}, {}};

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
        stopIfFailedOffDevice(wic->CreateBitmapScaler(&frameScaler), "WIC: не создалось уменьшение снимка");
        if (SUCCEEDED(frameScaler->Initialize(frame.Get(), fitted.width, fitted.height,
                                              WICBitmapInterpolationModeFant))) {
            pixelSource = frameScaler;
            shrunkOnFrame = true;
        }
    }

    // Конвертер — своё: остановка; его настройка на кадр — разбор чужих байт.
    ComPtr<IWICFormatConverter> converter;
    stopIfFailedOffDevice(wic->CreateFormatConverter(&converter), "WIC: не создался конвертер");
    if (FAILED(converter->Initialize(pixelSource.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        return notAnImage();
    pixelSource = converter;

    if (tooWide && !shrunkOnFrame) {
        ComPtr<IWICBitmapScaler> scaler;
        stopIfFailedOffDevice(wic->CreateBitmapScaler(&scaler), "WIC: не создалось уменьшение снимка");
        stopIfFailedOffDevice(scaler->Initialize(pixelSource.Get(), fitted.width, fitted.height,
                                                 WICBitmapInterpolationModeFant),
                              "WIC: уменьшение снимка не настроилось");
        pixelSource = scaler;
    }

    // Пиксели — здесь, целиком, в свой буфер: битые данные отказывают
    // сейчас, словом о файле, а не потом, на каждом кадре, и рисунок читает
    // уже память, а не байты файла. Кадр — через уменьшение и конвертер —
    // копируется в буфер полосами по kBandRows строк: полоса i — строки
    // [i·kBandRows, min((i+1)·kBandRows, h)), stride·строк байт с отступа
    // i·kBandRows·stride, где stride = w·4. Между полосами раскодирование в
    // пуле спрашивает, не бросили ли его (throwIfStopRequested): ждущий,
    // отменив ожидание, свободен сразу, а тело, осиротев, дорабатывает лишь
    // до ближайшей полосы — вызов WIC посреди не обрывается — и не держит пул
    // до конца снимка. Отказ на любой полосе — отказ всей картинки:
    // недокопированный буфер выбрасывается, полкартинки не бывает.
    //
    // Полосой WIC читает не всякий кадр: прогрессивный JPEG и чересстрочные
    // PNG и GIF декодер раскодирует целиком на первой же полосе — вся их
    // работа ложится в один вызов, и отмена доходит до них только после него.
    // Известный долг: ускорить конец такого снимка нечем, кроме меньшего
    // бюджета. Требует замера и другое: полосы идут сверху вниз, но
    // уменьшение Fant может просить у кадра строки с перекрытием назад, и
    // если декодер JPEG на таком запросе начинает кадр заново, полосы стоят
    // дороже одного вызова на весь кадр.
    //
    // E_OUTOFMEMORY при копировании — остановка, как и нехватка памяти на сам
    // буфер. Бюджет ограничил только кадр в его собственном формате, и обычно
    // эта нехватка — беда системы; но прогрессивный JPEG держит по ходу ещё
    // коэффициенты (4:2:0 — 3 байта на пиксель, 4:4:4 — 6, CMYK — 8, в
    // пределах бюджета до 2 ГиБ), и тогда это остановка от данных — известная
    // цена бюджета, который пропускает снимки телефонов на 200 Мп. Прочие
    // отказы копирования — о файле.
    throwIfStopRequested(state);

    DecodedPixels pixels;
    pixels.width = fitted.width;
    pixels.height = fitted.height;
    pixels.stride = fitted.width * 4;
    pixels.bytes.reset(new (std::nothrow) std::byte[size_t{pixels.stride} * pixels.height]);
    if (pixels.empty()) stopIfFailedOffDevice(E_OUTOFMEMORY, "WIC: не хватило памяти на пиксели снимка");

    const uint32_t bandCount = (pixels.height + kBandRows - 1) / kBandRows;
    for (uint32_t band = 0; band < bandCount; ++band) {
        throwIfStopRequested(state);

        const uint32_t top = band * kBandRows;
        const uint32_t rows = std::min(kBandRows, pixels.height - top);
        const auto rect = WICRect{0, static_cast<INT>(top), static_cast<INT>(pixels.width), static_cast<INT>(rows)};
        BYTE* const into = reinterpret_cast<BYTE*>(pixels.bytes.get() + size_t{top} * pixels.stride);

        const HRESULT copied = pixelSource->CopyPixels(&rect, pixels.stride, pixels.stride * rows, into);
        if (copied == E_OUTOFMEMORY)
            stopIfFailedOffDevice(copied, "WIC: не хватило памяти на снимок в бюджете");
        if (FAILED(copied)) return notAnImage();

        if (afterBand) afterBand(band);
    }

    return DecodedPicture{PictureVerdict{}, std::move(pixels)};
}

}  // namespace

DecodedPicture decodeImage(std::string_view bytes, uint32_t maxSide) {
    if (bytes.empty()) return notAnImage();

    return decodeImage(newImagingFactory().Get(), bytes, maxSide);
}

DecodedPicture decodeImage(IWICImagingFactory* wic, std::string_view bytes, uint32_t maxSide) {
    return decodeWith(wic, bytes, maxSide, nullptr, {});
}

namespace {

/// Обе формы в пуле. Тело трогает только своё: байты, предел и ступень проб
/// — копии в нём, ответ — его значение. Фабрика, декодер, уменьшение и
/// конвертер заводятся и отпускаются на потоке пула, внутри тела, — наружу
/// уходят одни пиксели. Ожидание стоит под токеном вызывающего: отменённое,
/// оно кончается отменой сразу, а тело сиротеет — доживает в пуле одно до
/// ближайшей полосы, и пиксели, которых никто не ждёт, отпускаются там же.
task<DecodedPicture> decodeInPool(std::string bytes, uint32_t maxSide, cancellation_token stop,
                                  std::function<void(uint32_t)> afterBand) {
    auto decoding = [bytes = std::move(bytes), maxSide,
                     afterBand = std::move(afterBand)](const orphan_state& state) -> DecodedPicture {
        if (bytes.empty()) return notAnImage();

        const ComPtr<IWICImagingFactory> wic = newImagingFactory();
        return decodeWith(wic.Get(), bytes, maxSide, &state, afterBand);
    };

    return sta_loop::async_call(orphanable, std::move(decoding), std::move(stop));
}

}  // namespace

task<DecodedPicture> decodeImageAsync(std::string bytes, uint32_t maxSide, cancellation_token stop) {
    return decodeInPool(std::move(bytes), maxSide, std::move(stop), {});
}

task<DecodedPicture> detail::decodeImageAsync(std::string bytes, uint32_t maxSide, cancellation_token stop,
                                              std::function<void(uint32_t band)> afterBand) {
    return decodeInPool(std::move(bytes), maxSide, std::move(stop), std::move(afterBand));
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

ComPtr<ID2D1Bitmap1> deviceBitmapOf(ID2D1DeviceContext* context, const DecodedPixels& pixels) {
    if (pixels.empty()) wxl::core::abort("deviceBitmapOf: снимка нет");

    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> bitmap;

    // Предел устройства известен заранее (на Windows 10 бывают устройства
    // уровня 9.x с пределом 2048–4096), и больший битмап Direct2D отверг бы —
    // это не сбой, а мера. Пиксели от decodeImage уже не больше
    // kMaxPictureSide, и обычно битмап делается прямо из них.
    const UINT32 deviceLimit = context->GetMaximumBitmapSize();
    if (pixels.width <= deviceLimit && pixels.height <= deviceLimit) {
        stopIfFailed(context->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height), pixels.bytes.get(),
                                           pixels.stride, properties, &bitmap),
                     "Direct2D: снимок не стал битмапом устройства");
        return bitmap;
    }

    // Ниже предела устройства — уменьшение WIC на этом потоке: битмап WIC из
    // копии пикселей (CreateBitmapFromMemory копирует, а неконстантный
    // указатель просит лишь по сигнатуре), Fant до предела, затем битмап
    // устройства. Отказы WIC — без проверки устройства, Direct2D — с ней.
    const PictureSize fitted = fittedSize(pixels.width, pixels.height, deviceLimit);
    const ComPtr<IWICImagingFactory> wic = newImagingFactory();
    ComPtr<IWICBitmap> source;
    stopIfFailedOffDevice(wic->CreateBitmapFromMemory(pixels.width, pixels.height, GUID_WICPixelFormat32bppPBGRA,
                                                      pixels.stride, pixels.stride * pixels.height,
                                                      reinterpret_cast<BYTE*>(pixels.bytes.get()), &source),
                          "WIC: пиксели снимка не стали битмапом для уменьшения");
    ComPtr<IWICBitmapScaler> scaler;
    stopIfFailedOffDevice(wic->CreateBitmapScaler(&scaler), "WIC: не создалось уменьшение снимка");
    stopIfFailedOffDevice(scaler->Initialize(source.Get(), fitted.width, fitted.height, WICBitmapInterpolationModeFant),
                          "WIC: снимок не уменьшился до предела устройства");
    stopIfFailed(context->CreateBitmapFromWicBitmap(scaler.Get(), &properties, &bitmap),
                 "Direct2D: снимок не стал битмапом устройства");
    return bitmap;
}

}  // namespace bukvitsa::reader
