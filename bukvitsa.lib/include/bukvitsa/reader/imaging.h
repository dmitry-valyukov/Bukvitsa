#pragma once
// Раскодирование картинки — общая ступень подложки темы, мастера обложек и
// картинок книги: WIC раскодирует снимок сразу и целиком в битмап памяти
// 32bppPBGRA, а битмап устройства из него делает уже тот контекст, который
// будет рисовать. Диска здесь нет: байты файла приносит
// `async_file::read_all`, и раскодируется уже память.
//
// Целиком в одном вызове, а не конвертером: ленивый конвертер читает пиксели
// внутри CreateBitmapFromWicBitmap, и байты, битые не в заголовке, а в данных,
// отказывали бы на каждом кадре — не отличить от отказа Direct2D.
// Раскодированный целиком снимок отказывает при рисовании только от самого
// Direct2D. Когда раскодировать, решает вызывающий: подложка — сразу, она
// одна; картинки книги — при первом рисунке каждой (`Book::bitmap`), а при
// открытии — только размеры из заголовка (`pictureSizeOf`).

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

#include <d2d1_1.h>
#include <wrl/client.h>

struct IWICBitmap;
struct IWICBitmapSource;
struct IWICImagingFactory;

namespace bukvitsa::reader {

/// Предел раскодирования по большей стороне. Страница не больше экрана, и
/// 4096 пикселей покрывают большую сторону страницы на 4K при любом масштабе;
/// раскодированная картинка тогда не больше 64 МБ. Большее уменьшается при
/// раскодировании — до выделения памяти под пиксели: размеру из заголовка
/// файла не верят (PNG в килобайты может объявить 30000×30000), а снимок с
/// телефона на 200 Мп иначе стал бы битмапом устройства в сотни мегабайт.
inline constexpr uint32_t kMaxPictureSide = 4096;

/// Размер картинки в пикселях.
struct PictureSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

/// Почему байты не стали снимком.
enum class PictureRefusal {
    None,         ///< стали
    NotAnImage,   ///< не картинка, которую знает WIC (или нулевая сторона)
    TooLarge,     ///< картинка, но больше бюджета: размер известен (`declaredSize`)
};

/// Стали ли байты снимком, а нет — почему.
struct PictureVerdict {
    PictureRefusal refusal = PictureRefusal::None;
    PictureSize declaredSize;   ///< размер из заголовка — у `TooLarge`
};

/// Ответ раскодирования: снимок — или почему его нет.
struct DecodedPicture {
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;   ///< есть — только при `PictureRefusal::None`
    PictureVerdict verdict;
};

/// Снимок, раскодированный целиком и не больше `maxSide` по большей стороне
/// (больший уменьшается, Fant, с сохранением пропорций), — или почему его
/// нет. Байты после вызова не нужны: пиксели уже в битмапе. Фабрика WIC своя
/// и на один вызов: снимки раскодируются по смене темы и по выбору снимка в
/// мастере, а не в цикле.
DecodedPicture decodeImage(std::string_view bytes, uint32_t maxSide = kMaxPictureSide);

/// То же фабрикой вызывающего — у книги она одна на все её картинки. Есть
/// контекст — предел `min(kMaxPictureSide, GetMaximumBitmapSize())`.
DecodedPicture decodeImage(IWICImagingFactory* wic, std::string_view bytes, uint32_t maxSide = kMaxPictureSide);

/// Бюджет памяти картинки по заголовку — до всякого раскодирования, в байтах:
/// декодер может держать полный буфер в своём формате (16384² в 16-битном
/// RGBA, чересстрочный, — 2 ГиБ, в 128bppRGBAFloat — 4 ГиБ), а уменьшение
/// (Fant) сжимает только выход. Считается w·h·max(бит на пиксель, 32)/8 —
/// не меньше, чем займёт PBGRA, — и 2³⁰ (1 ГиБ) пропускает снимок телефона на
/// 200 Мп (16320×12240 в 24 или 32 бита — около 800 МБ) и квадрат 16384² в 32
/// бита. В бюджете отказ раскодирования по нехватке памяти — уже беда
/// системы, а не файла.
inline constexpr uint64_t kMaxPictureBytes = uint64_t{1} << 30;

/// Укладывается ли картинка такого размера и формата из заголовка в бюджет:
/// стороны не нулевые, байты — не больше `kMaxPictureBytes` (в uint64_t, без
/// переполнения).
constexpr bool withinPictureBudget(uint32_t width, uint32_t height, uint32_t bitsPerPixel) noexcept {
    if (width == 0 || height == 0) return false;
    const uint64_t bytesPerPixel = (std::max<uint64_t>(bitsPerPixel, 32) + 7) / 8;
    return uint64_t{width} * height <= kMaxPictureBytes / bytesPerPixel;
}

/// Размер, уменьшенный до `limit` по большей стороне с сохранением пропорций:
/// большая сторона — ровно `limit`, меньшая — целочисленно, не меньше
/// пикселя. Картинка не больше предела — как есть: увеличивать незачем.
/// Нулевая сторона или `limit` — остановка.
PictureSize fittedSize(uint32_t width, uint32_t height, uint32_t limit);

/// Заголовок картинки, без пикселей.
struct PictureHeader {
    PictureSize size;
    bool tooLarge = false;   ///< больше бюджета: раскодировать её не станут
};

/// Размер по заголовку; пусто — байты не картинка или у неё нулевая
/// сторона. Отказы — те же, что у `decodeImage`: своё — остановка, разбор
/// чужих байт — пусто.
std::optional<PictureHeader> pictureSizeOf(IWICImagingFactory* wic, std::string_view bytes);

/// Битмап устройства из раскодированного снимка — не больше, чем устройство
/// держит одним битмапом (`GetMaximumBitmapSize`). Снимок от `decodeImage`
/// уже не больше `kMaxPictureSide`, так что уменьшение здесь (Fant, с
/// сохранением пропорций) — лишь для устройства с пределом ниже. Отказ
/// Direct2D или WIC здесь — остановка: байты уже раскодированы, размер
/// ограничен, и отказать может лишь система.
Microsoft::WRL::ComPtr<ID2D1Bitmap1> deviceBitmapOf(ID2D1DeviceContext* context, IWICBitmapSource* picture);

}  // namespace bukvitsa::reader
