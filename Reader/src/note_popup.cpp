#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "note_popup.h"

#include "Bind.h"

#include "bukvitsa/reader/note_frame.h"
#include "bukvitsa/reader/note_scroll.h"
#include "bukvitsa/typography/glyph_painter.h"
#include "bukvitsa/typography/system_failure.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

constexpr float kCorner = 10.0f;
constexpr float kMaxWidth = 460.0f;
constexpr float kGapFromAnchor = 10.0f;

/// Доля высоты полосы, выше которой всплывашка не растёт. Сноска, занявшая
/// пол-экрана, перестаёт быть примечанием и становится второй страницей — а
/// прокрутка внутри неё дешевле, чем потерянное место чтения.
constexpr float kMaxHeightShare = 0.55f;

/// Высота полосы, которую рисует один вызов поверхности: 1024 пикселя — не
/// выше предела любого устройства (2045 на уровне 9_1), так что предел её не
/// режет, а полоса во всю ширину сноски при масштабе 1,5 (686 пикселей) — 2,8 МБ.
constexpr int32_t kBandPixels = 1024;

/// Место прокрутки в DIP — в пиксели поверхности, вниз: верх окна не должен
/// оказаться ниже того, что показано.
int32_t pixelsOf(double dips, float scale) {
    constexpr auto most = static_cast<double>(VirtualDrawingSurface::maxSide);
    return static_cast<int32_t>(std::clamp(std::floor(dips * scale), 0.0, most));
}

/// Цвет D2D в цвет XAML: подложку рисует не наша поверхность, а Border, —
/// у него уже есть и скругление, и рамка, и незачем чертить их руками.
Color toXaml(D2D1_COLOR_F color) {
    auto byte = [](float value) {
        return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return Color{byte(color.a), byte(color.r), byte(color.g), byte(color.b)};
}

}  // namespace

// Спрайт текста — сразу, от композитора: поверхность под него заводит
// первый показ (prepareSurface), тогда же он и получает кисть.
NotePopup::NotePopup(const Compositor& compositor)
    : compositor_(compositor), sprite_(compositor_.createSpriteVisual()), deviceWatch_(compositor_, *this) {
    using namespace wxl::dsl;

    // Подложка ростом с текст сноски и полем над ним и под ним: она внутри
    // прокрутки, и её высота — это высота сноски. Сам текст живёт на
    // поверхности композитора, привязанной к этому элементу, поэтому прокрутка
    // двигает его вместе с ним.
    ElementCompositionPreview::setElementChildVisual(paper_, sprite_);

    Apply {
        root_,
        hAlign.left,
        vAlign.top,
        visibility = Visibility::Collapsed,
        CornerRadius{kCorner},
        // Поля у рамки нет: поле вокруг тела — внутри окна прокрутки, на
        // поверхности (note_frame.h). Окно режет всё, что за его краем, и срез
        // приходится на край поля, а не на край колонки.
        BorderThickness{kNoteBorder},

        // Сноска — не страница: нажатие и колесо по ней здесь и кончаются, как
        // у ящиков панели. Всплыв к полосе, нажатие ушло бы ведущему щелчком
        // по странице под сноской — открыло бы знак, который она закрывает,
        // или спрятало бы её саму, а правой кнопкой открыло бы ящики. Колесо
        // сюда доходит, только когда прокрутке листать нечего — сноска
        // короткая или докручена, — и у полосы оно перелистнуло бы страницу,
        // спрятав сноску, а с Ctrl сменило бы кегль.
        onPointerPressed += [](Object const&, PointerRoutedEventArgs& args) { args.handled(true); },
        onPointerReleased += [](Object const&, PointerRoutedEventArgs& args) { args.handled(true); },
        onPointerWheelChanged += [](Object const&, PointerRoutedEventArgs& args) {
            args.handled(true);
        },

        ScrollViewer {
            horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
            verticalScrollBarVisibility = ScrollBarVisibility::Auto,
            verticalOffset = Bind{scrollTop_},
            onViewChanging += method(this, &NotePopup::viewChanging),
            content = paper_,
        },
    };
}

void NotePopup::hide() {
    if (!visible_) return;
    visible_ = false;
    root_.visibility(Visibility::Collapsed);
}

void NotePopup::scroll(NoteScroll noteScroll) {
    if (!visible_) core::abort("NotePopup: прокрутка скрытой сноски");

    // Окно и подложка — в DIP из пикселей разметки (shownFrame_), а не из роста
    // в DIP: по пикселям мера обещает, что окно короткой сноски не ниже
    // подложки, и короткая здесь та же, что у окна прокрутки.
    //
    // Шаг — от места, где прокрутка встанет (goalTop_), а не от поля
    // scrollTop_: поле узнаёт место только от вставшей прокрутки, и посреди
    // инерции колеса оно ещё на прежнем месте — PgDn шагнул бы от него назад.
    // После своей просьбы цель — то, что в поле: привязка обрезает её до конца
    // прокрутки, и следующая клавиша, нажатая раньше, чем прокрутка скажет о
    // себе, шагает от обрезанной.
    //
    // Известное ограничение: поле, равное новой цели, привязка не слышит, и
    // если прокрутка в полёте, а поле ещё стоит ровно там, куда шагнула
    // клавиша, прокрутка к нему не вернётся — клавиша пропадёт. Просьбы к
    // прокрутке при равном поле у привязки wxl нет.
    const double windowHeight = static_cast<double>(shownFrame_.windowHeight) / scale_;
    const double paperHeight = static_cast<double>(shownFrame_.paperPixels) / scale_;
    scrollTop_.set(noteScrollTarget(body_.lines, goalTop_, windowHeight, paperHeight, noteScroll));
    goalTop_ = scrollTop_.get();
}

void NotePopup::renderingDeviceReplaced() noexcept {
    if (visible_) startBands();
}

void NotePopup::viewChanging(const ScrollViewer&, ScrollViewerViewChangingEventArgs& args) {
    if (!visible_) return;
    viewTop_ = args.nextView().verticalOffset();
    goalTop_ = args.finalView().verticalOffset();
    applyBands(bands_.advance(
        {pixelsOf(viewTop_, scale_), pixelsOf(args.finalView().verticalOffset(), scale_)}));
}

void NotePopup::prepareSurface() noexcept {
    auto const pixels = SizeInt32{shownFrame_.windowWidth, shownFrame_.paperPixels};
    if (surface_) {
        surface_->resize(pixels);
        // Что прежняя сноска нарисовала в новых границах, решётка новой не
        // знает: отдаётся всё.
        surface_->trim({});
    } else {
        surface_.emplace(compositor_, pixels);
        sprite_.brush(surface_->brush());
    }
    // Спрайт — ровно в пиксели поверхности, а не в DIP окна: кисть поверхности
    // растягивает её на спрайт (CompositionStretch::Fill по умолчанию), и спрайт
    // мимо целых пикселей пересэмплировал бы текст на долю пикселя — при
    // масштабе 1,25 и 1,75 он вышел бы мягким.
    sprite_.size({static_cast<float>(pixels.width) / scale_, static_cast<float>(pixels.height) / scale_});
}

void NotePopup::startBands() noexcept {
    const SizeInt32 pixels = surface_->size();
    bands_.reset({.surfaceWidth = pixels.width,
                  .surfaceHeight = pixels.height,
                  .viewportHeight = shownFrame_.windowHeight,
                  .bandHeight = kBandPixels,
                  .maxUpdateSide = surface_->maxUpdateSide()});
    applyBands(bands_.advance({pixelsOf(viewTop_, scale_)}));
}

// Сперва отдать, потом рисовать: отданное освобождает память раньше, чем её
// займёт новое, а новые полосы в keepRects ещё не нарисованы — их отдача
// ничего не стоит.
void NotePopup::applyBands(const NoteBandsStep& step) noexcept {
    if (step.trimNeeded) {
        std::vector<RectInt32> keptParts;
        keptParts.reserve(step.keepRects.size());
        for (const NotePixelRect& keptRect : step.keepRects) {
            keptParts.push_back(RectInt32{keptRect.x, keptRect.y, keptRect.width, keptRect.height});
        }
        surface_->trim(keptParts);
    }
    for (const NotePixelRect& newPiece : step.drawRects) drawPiece(newPiece);
}

void NotePopup::drawPiece(const NotePixelRect& updatePiece) noexcept {
    const float scale = scale_;
    // Кусок в DIP сноски. Строка берётся с запасом в свою высоту с обеих
    // сторон: верх строки — подъём шрифтов её прогонов, а глифы надстрочного
    // знака сдвинуты выше него (ascenderOffset), и чернила строки у края
    // полосы иначе срезались бы клипом куска.
    const float pieceTop = static_cast<float>(updatePiece.y) / scale;
    const float pieceBottom = static_cast<float>(updatePiece.y + updatePiece.height) / scale;

    auto const updateRect =
        RectInt32{updatePiece.x, updatePiece.y, updatePiece.width, updatePiece.height};
    surface_->draw(updateRect, [&](ID2D1DeviceContext* context) {
        D2D1_MATRIX_3X2_F surfacePlace{};
        context->GetTransform(&surfacePlace);
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                              *D2D1::Matrix3x2F::ReinterpretBaseType(&surfacePlace));

        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Отказ — остановка (system_failure.h): молча пустая сноска была бы
        // сбоем без имени.
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ink;
        typography::stopIfFailed(context->CreateSolidColorBrush(ink_, &ink),
                                 "NotePopup: не создалась кисть сноски");

        // Знак сноски внутри сноски — цветом текста, а не ссылки: всплывашка
        // знаков не ловит, и цвет ссылки обещал бы щелчок, которого нет.
        auto const noteBrushes = typography::TextBrushes{not_null<ID2D1Brush>{ink.Get()},
                                                         not_null<ID2D1Brush>{ink.Get()}};
        // Тело — в поле от края подложки: слева и сверху kNotePadding.
        for (const typography::NoteLine& placed : body_.lines) {
            const typography::Line& line = placed.line;
            const float baseline = kNotePadding + placed.baseline;
            if (baseline + line.descent + line.height <= pieceTop) continue;
            if (baseline - line.ascent - line.height >= pieceBottom) continue;
            typography::drawLine(context, line, kNotePadding, baseline, noteBrushes);
        }
    });
}

void NotePopup::show(Book& book, uint32_t body, Point anchor, Size area, const Theme& theme,
                     float fontSize, float scale) {
    // Прежняя сноска снимается первой, новая показывается последней: сброс места
    // прокрутки ниже поднимает ViewChanging тут же, внутри show (без DManip —
    // ScrollData::put_OffsetY), и показанной слушатель рисовал бы новое тело по
    // прежним масштабу, решётке и поверхности; скрытую он не трогает.
    hide();

    const float width = std::min(kMaxWidth, std::max(area.width * 0.6f, 220.0f));

    // Тело верстается из хранилища книги: его блоки развёрнуты там вместе с
    // потоком чтения, заново ничего не разворачивается. Колонка — всплывашка
    // без двух рамок и двух полей; поле остаётся внутри окна прокрутки
    // (note_frame.h).
    const typography::BookText& text = book.text();
    body_ = typography::layoutNote(book.engine(), text, text.noteBodies[body], noteColumn(width),
                                   fontSize);
    if (body_.lines.empty()) return;

    // Мера — с округлением разметки до пикселей этого экрана: окно прокрутки
    // и поверхность в целых пикселях, а рост — такой, что окно не ниже
    // подложки, пока тело помещается в долю полосы.
    const NoteFrame popupFrame =
        noteFrame(width, body_.height, std::max(area.height * kMaxHeightShare, 120.0f), scale);
    const float height = popupFrame.popupHeight;

    // Встать под знаком сноски, а если внизу не помещается — над ним: так
    // всплывашка не закрывает того места, ради которого её открыли. Не
    // помещается и над ним — прижимается к верху полосы в 8 DIP от края и
    // тогда встаёт на знак: высота у неё уже ограничена долей полосы, и
    // показать сноску целиком важнее, чем не заслонить знак.
    float x = std::clamp(anchor.x - width * 0.5f, 8.0f, std::max(area.width - width - 8.0f, 8.0f));
    float y = anchor.y + kGapFromAnchor;
    if (y + height > area.height - 8.0f) {
        y = std::max(anchor.y - height - kGapFromAnchor, 8.0f);
    }

    root_.margin({x, y, 0, 0});
    root_.width(width);
    root_.height(height);
    root_.background(SolidColorBrush{toXaml(theme.panel)});
    root_.borderBrush(SolidColorBrush{toXaml(theme.dim)});

    paper_.height(popupFrame.paperHeight);
    // Новая сноска — с начала, а не с того места, до которого прокрутили
    // прежнюю.
    scrollTop_.set(0.0);

    ink_ = theme.text;
    // Окно прокрутки — из меры, а не у прокрутки: разметки ещё не было, и
    // спрашивать его у прокрутки рано.
    shownFrame_ = popupFrame;
    scale_ = scale;
    // Вид — там, куда сброс привёл прокрутку: поле говорит это само.
    viewTop_ = scrollTop_.get();
    goalTop_ = viewTop_;
    prepareSurface();
    startBands();

    root_.visibility(Visibility::Visible);
    visible_ = true;
}

}  // namespace bukvitsa::reader
