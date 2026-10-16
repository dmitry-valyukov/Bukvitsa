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

#include <cstdint>
#include <optional>
#include <string_view>

#include <d2d1_1.h>
#include <wrl/client.h>

struct IWICBitmap;
struct IWICBitmapSource;
struct IWICImagingFactory;

namespace bukvitsa::reader {

/// Снимок, раскодированный целиком, — или nullptr, если байты не картинка.
/// Байты после вызова не нужны: пиксели уже в битмапе. Фабрика WIC своя и на
/// один вызов: снимки раскодируются по смене темы и по выбору снимка в
/// мастере, а не в цикле.
Microsoft::WRL::ComPtr<IWICBitmap> decodeImage(std::string_view bytes);

/// То же фабрикой вызывающего — у книги она одна на все её картинки.
Microsoft::WRL::ComPtr<IWICBitmap> decodeImage(IWICImagingFactory* wic, std::string_view bytes);

/// Размер картинки в пикселях.
struct PictureSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

/// Размер по заголовку, без пикселей; пусто — байты не картинка. Отказы — те
/// же, что у `decodeImage`: своё — остановка, разбор чужих байт — пусто.
std::optional<PictureSize> pictureSizeOf(IWICImagingFactory* wic, std::string_view bytes);

/// Битмап устройства из раскодированного снимка — не больше, чем устройство
/// держит одним битмапом (`GetMaximumBitmapSize`): больший уменьшается
/// (Fant) с сохранением пропорций до предела по большей стороне — страница
/// меньше предела, и видимого он не теряет. Отказ Direct2D или WIC здесь —
/// остановка: байты уже раскодированы, и отказать может лишь система.
Microsoft::WRL::ComPtr<ID2D1Bitmap1> deviceBitmapOf(ID2D1DeviceContext* context, IWICBitmapSource* picture);

}  // namespace bukvitsa::reader
