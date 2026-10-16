#include <algorithm>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "note_popup.h"

#include "Bind.h"

#include "bukvitsa/typography/glyph_painter.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

constexpr float kPadding = 16.0f;
constexpr float kCorner = 10.0f;
constexpr float kMaxWidth = 460.0f;
constexpr float kGapFromAnchor = 10.0f;

/// Доля высоты полосы, выше которой всплывашка не растёт. Сноска, занявшая
/// пол-экрана, перестаёт быть примечанием и становится второй страницей — а
/// прокрутка внутри неё дешевле, чем потерянное место чтения.
constexpr float kMaxHeightShare = 0.55f;

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
// первый показ (draw), тогда же он и получает кисть.
NotePopup::NotePopup(const Compositor& compositor)
    : compositor_(compositor), sprite_(compositor_.createSpriteVisual()) {
    using namespace wxl::dsl;

    // Подложка ростом с текст сноски: она внутри прокрутки, и её высота — это
    // высота сноски. Сам текст живёт на поверхности композитора, привязанной
    // к этому элементу, поэтому прокрутка двигает его вместе с ним.
    ElementCompositionPreview::setElementChildVisual(paper_, sprite_);

    Apply {
        root_,
        hAlign.left,
        vAlign.top,
        visibility = Visibility::Collapsed,
        CornerRadius{kCorner},
        BorderThickness{1},
        Padding{kPadding},

        // Сноска — не страница: нажатие и колесо по ней здесь и кончаются, как
        // у ящиков панели. Всплыв к полосе, нажатие ушло бы ведущему щелчком
        // по странице под сноской — открыло бы знак, который она закрывает,
        // или спрятало бы её саму, а правой кнопкой открыло бы ящики. Колесо
        // сюда доходит, только когда прокрутке листать нечего — сноска
        // короткая или докручена, — и у полосы оно перелистнуло бы страницу,
        // спрятав сноску, а с Ctrl сменило бы кегль.
        onPointerPressed = [](Object const&, PointerRoutedEventArgs& args) { args.handled(true); },
        onPointerReleased = [](Object const&, PointerRoutedEventArgs& args) { args.handled(true); },
        onPointerWheelChanged = [](Object const&, PointerRoutedEventArgs& args) {
            args.handled(true);
        },

        ScrollViewer {
            horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
            verticalScrollBarVisibility = ScrollBarVisibility::Auto,
            verticalOffset = Bind{scrollTop_},
            content = paper_,
        },
    };
}

void NotePopup::hide() {
    if (!visible_) return;
    visible_ = false;
    root_.visibility(Visibility::Collapsed);
}

void NotePopup::draw(const Theme& theme, float width, float height, float scale) {
    const SizeInt32 pixels{static_cast<int32_t>(width * scale + 0.5f),
                           static_cast<int32_t>(height * scale + 0.5f)};
    if (pixels.width <= 0 || pixels.height <= 0) return;

    if (surface_) {
        surface_->resize(pixels);
    } else {
        surface_.emplace(compositor_, pixels);
        sprite_.brush(surface_->brush());
    }
    sprite_.size({width, height});

    surface_->draw([&](ID2D1DeviceContext* context) {
        D2D1_MATRIX_3X2_F atlas{};
        context->GetTransform(&atlas);
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                              *D2D1::Matrix3x2F::ReinterpretBaseType(&atlas));

        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        context->Clear(D2D1::ColorF(0, 0, 0, 0));   // подложку рисует Border

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ink;
        context->CreateSolidColorBrush(theme.text, &ink);
        if (!ink) return;

        // Знак сноски внутри сноски — цветом текста, а не ссылки: всплывашка
        // знаков не ловит, и цвет ссылки обещал бы щелчок, которого нет.
        auto const noteBrushes = typography::TextBrushes{not_null<ID2D1Brush>{ink.Get()},
                                                         not_null<ID2D1Brush>{ink.Get()}};
        for (const typography::NoteLine& placed : body_.lines) {
            typography::drawLine(context, placed.line, 0.0f, placed.baseline, noteBrushes);
        }
    });
}

void NotePopup::show(Book& book, uint32_t body, Point anchor, Size area, const Theme& theme,
                     float fontSize, float scale) {
    const float width = std::min(kMaxWidth, std::max(area.width * 0.6f, 220.0f));
    const float inner = width - kPadding * 2.0f;

    // Тело верстается из хранилища книги: его блоки развёрнуты там вместе с
    // потоком чтения, заново ничего не разворачивается.
    const typography::BookText& text = book.text();
    body_ = typography::layoutNote(book.engine(), text, text.noteBodies[body], inner, fontSize);
    if (body_.lines.empty()) {
        hide();
        return;
    }
    const float noteHeight = body_.height;

    const float height =
        std::min(noteHeight + kPadding * 2.0f, std::max(area.height * kMaxHeightShare, 120.0f));

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

    paper_.height(noteHeight);
    // Новая сноска — с начала, а не с того места, до которого прокрутили
    // прежнюю.
    scrollTop_.set(0.0);

    draw(theme, inner, noteHeight, scale);

    root_.visibility(Visibility::Visible);
    visible_ = true;
}

}  // namespace bukvitsa::reader
