#include <algorithm>
#include <iterator>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "bukvitsa/reader/book.h"

#include "bukvitsa/typography/block.h"
#include "bukvitsa/typography/system_failure.h"

namespace bukvitsa::reader {

using Microsoft::WRL::ComPtr;

namespace {

/// Раскодирует картинку до конвертера в 32bppPBGRA и снимает её размеры: они
/// нужны пагинатору до всякой отрисовки. На видеокарту она не идёт — битмап
/// создаётся позже, на том устройстве, которое будет рисовать. Поток WIC —
/// поверх байтов самой картинки, без копии.
/// @return false — байтов нет или это не картинка, которую WIC знает.
bool decodeInto(IWICImagingFactory* wic, ImageAsset& asset) {
    if (asset.bytes.empty()) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(asset.bytes.data()),
                                            static_cast<DWORD>(asset.bytes.size()))))
        return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                            &decoder)))
        return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)))
        return false;

    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height))) return false;

    asset.width = static_cast<float>(width);
    asset.height = static_cast<float>(height);
    asset.source = std::move(converter);
    return true;
}

}  // namespace

IDWriteFactory* dwriteFactory() {
    // Живёт до конца процесса и намеренно не освобождается: разделяемую
    // фабрику DirectWrite всё равно держит система, а порядок разрушения
    // статиков на выходе нам не подконтролен.
    static IDWriteFactory* factory = nullptr;
    if (!factory) {
        // Системная фабрика не создаётся лишь от беды самой системы: без неё нет
        // ни вёрстки, ни колонцифры, и ответить на это нечем — остановка, а не
        // пустой указатель и не «книга не открылась».
        typography::stopIfFailed(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                     reinterpret_cast<IUnknown**>(&factory)),
                                 "DirectWrite: не создалась фабрика");
    }
    return factory;
}

Book::Book(const std::filesystem::path& path, fb3::Document&& document, IDWriteFactory* dwrite)
    : path_(path), engine_(dwrite), text_(typography::flatten(document)),
      characterCount_(document.characterCount()),
      // Пагинатор спрашивает размеры картинок у нас: вёрстка их не декодирует и
      // про WIC не знает. Главы заводятся по требованию (chapterAt), то есть
      // после тела конструктора, которое картинки и раскодирует.
      imageSize_([this](uint32_t index) {
          if (index >= images_.size())
              return typography::ImageSize{};
          return typography::ImageSize{images_[index].width, images_[index].height};
      }) {
    decodeImages(document);

    // Границы глав верхнего уровня: начало книги и каждый блок потока,
    // открывающий секцию верхнего уровня. По ним поток режется на главы; тела
    // сносок лежат за ним и в главы не входят.
    const std::span<const typography::Block> flow = text_.flow();
    if (!flow.empty()) {
        chapterStarts_.push_back(0);
        for (size_t i = 1; i < flow.size(); ++i)
            if (flow[i].startsSection == 1)
                chapterStarts_.push_back(i);
    }
}

Book::~Book() = default;

bool Book::setCurrentChapter(uint32_t charOffset) {
    if (chapterStarts_.empty())
        return false;

    // Блок потока, внутри которого лежит символ, — последний, начинающийся не
    // позже. Тела сносок за потоком сюда не попадают: в главах их нет.
    const std::span<const typography::Block> flow = text_.flow();
    const auto blockIt = std::upper_bound(
        flow.begin(), flow.end(), charOffset,
        [](uint32_t off, const typography::Block& b) { return off < b.charOffset; });
    const size_t block =
        blockIt == flow.begin()
            ? 0
            : static_cast<size_t>(std::distance(flow.begin(), blockIt) - 1);

    // Глава — последнее её начало не позже этого блока.
    const auto chapterIt =
        std::upper_bound(chapterStarts_.begin(), chapterStarts_.end(), block);
    const size_t chapter =
        static_cast<size_t>(std::distance(chapterStarts_.begin(), chapterIt) - 1);

    if (chapter == currentChapter_)
        return false;

    currentChapter_ = chapter;
    // Заводим её в кэше (шейпинг соседних при этом не пропадает) и сбрасываем
    // раскладку под свежий стиль — переложит её читалка.
    chapterAt(chapter).resetLayout();
    return true;
}

std::span<const typography::Block> Book::chapterSpan(size_t index) const {
    const std::span<const typography::Block> flow = text_.flow();
    const size_t first = chapterStarts_[index];
    const size_t last =
        index + 1 < chapterStarts_.size() ? chapterStarts_[index + 1] : flow.size();
    return flow.subspan(first, last - first);
}

typography::Chapter& Book::chapterAt(size_t index) {
    if (auto it = chapters_.find(index); it != chapters_.end())
        return *it->second;

    // Ничего не выбрасываем: разворот на стыке держит указатели в страницы
    // сразу нескольких глав, и уронить любую из них — висячий указатель.
    // Чистит кэш trimChapters, когда разворот уже собран.
    auto [pos, inserted] = chapters_.emplace(
        index, std::make_unique<typography::Chapter>(engine_, text_, chapterSpan(index),
                                                     characterCount_, imageSize_));
    return *pos->second;
}

void Book::trimChapters(size_t keepRadius) {
    // До первой наводки текущей главы нет — трогать нечего, а расстояние до
    // npos переполнилось бы и вымело весь кэш.
    if (currentChapter_ == static_cast<size_t>(-1))
        return;

    for (auto it = chapters_.begin(); it != chapters_.end();) {
        const size_t dist = it->first > currentChapter_ ? it->first - currentChapter_
                                                             : currentChapter_ - it->first;
        if (dist > keepRadius)
            it = chapters_.erase(it);
        else
            ++it;
    }
}

void Book::decodeImages(fb3::Document& document) {
    // Размер — один раз и до первых байтов: поток WIC смотрит в байты
    // элемента, и после этого элементам переезжать нельзя.
    const size_t count = document.images().size();
    images_.resize(count);

    if (count == 0)
        return;

    // Системная фабрика — не ответ о книге: не создалась — остановка, а не
    // книга без иллюстраций.
    typography::stopIfFailed(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)),
        "Book: не создалась фабрика WIC");

    for (uint32_t index = 0; index < count; ++index) {
        ImageAsset& asset = images_[index];

        // Байты переезжают в книгу до декодирования, и поток WIC встаёт уже на
        // них: он читает свою память, пока жив конвертер, а конвертер книга
        // держит до конца — битмап из него делается при отрисовке, — документ
        // же столько не живёт. Вёрстке нужны размеры всех картинок сразу — без
        // них не построить страницу, — так что лениться тут не выходит.
        asset.bytes = document.takeImageBytes(index);

        if (!decodeInto(wic_.Get(), asset))
            asset.bytes = std::string{};   // не картинка — рисовать нечего, и байты ни к чему
    }
}

ID2D1Bitmap1* Book::bitmap(uint32_t index, ID2D1DeviceContext* context) {
    if (index >= images_.size())
        return nullptr;

    ImageAsset& asset = images_[index];
    if (asset.bitmap)
        return asset.bitmap.Get();
    if (!asset.source)
        return nullptr;

    if (FAILED(context->CreateBitmapFromWicBitmap(asset.source.Get(), nullptr, &asset.bitmap)))
        asset.source.Reset();   // не вышло — больше не пытаемся

    return asset.bitmap.Get();
}

}  // namespace bukvitsa::reader
