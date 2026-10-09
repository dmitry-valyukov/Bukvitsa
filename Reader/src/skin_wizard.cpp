#include <algorithm>

#include <d2d1_1.h>

// Заголовки проекта после стандартных. Свой первым.
#include "skin_wizard.h"

#include "look.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

// Цвет для Direct2D из той же записи `rgb`/`rgba`, что у XAML: одна запись
// цвета на всё дерево, и редактор узнаёт её и ставит рядом образец.
constexpr D2D1_COLOR_F colorF(Color color) {
    return {color.R / 255.0f, color.G / 255.0f, color.B / 255.0f, color.A / 255.0f};
}

// Сетка поверх страницы — подсказка, а не занавес: все линии сильно
// полупрозрачны, центральная ярче тоном, чтобы читаться сквозь текст.
// Кружочки полупрозрачны, как кнопки: под ними тоже страница.
constexpr D2D1_COLOR_F kCurveColor = colorF(rgba(255, 217, 115, 0.6));
constexpr D2D1_COLOR_F kGripFill = colorF(rgba(255, 255, 255, 0.55));
constexpr D2D1_COLOR_F kGripRing = colorF(rgba(38, 31, 20, 0.7));

/// Тень кривой: две чёрно-коричневые полупрозрачные линии в пиксель над и
/// под основной — они оттеняют её на светлой бумаге.
constexpr D2D1_COLOR_F kCurveShade = colorF(rgba(28, 18, 8, 0.3));

// Зона захвата шире кружочка — она у модели правки (`SkinEditor::kGripReach`).
constexpr float kGripRadius = 7.0f;   ///< рисуемый кружочек, DIP
constexpr float kCurveStep = 4.0f;    ///< шаг ломаной, которой рисуется кривая

/// Сколько линий-подсказок между верхней и нижней кривыми, считая их самих.
constexpr int kGuideRows = 9;

constexpr size_t kSpinePoint = static_cast<size_t>(EdgeCurve::kSpine);

/// Точка указателя на сетке — в тип модели правки, которая окна не знает.
SkinEditor::Point onGrid(Point point) {
    return {point.x, point.y};
}

/// Кнопка карточки — полупрозрачная, как на стартовом экране, только без
/// анимации появления: мастер открывают действием, ждать ему нечего.
/// Прозрачность — визуалом, а не тегом, тоже как там.
Button translucent(Button button) {
    ElementCompositionPreview::getElementVisual(button).opacity(kOverlayButtonOpacity);
    return button;
}

}  // namespace

// Визуал сетки — сразу, от композитора: поверхность под него заводит первый
// размер окна (resizeSurface), тогда же он и встаёт в дерево.
SkinWizard::SkinWizard(const Compositor& compositor)
    : compositor_(compositor), visual_(compositor_.createSpriteVisual()) {
    buildTree();
}

void SkinWizard::buildTree() {
    using namespace wxl::dsl;

    // Кнопки — на той же карточке и на том же месте, что у стартового
    // экрана, и вида того же (look.h): «Сохранить» увеличена, как
    // «Продолжить чтение», — это действие по умолчанию, его же зовёт Enter;
    // «Выйти из мастера обложек» — отмена, её зовёт Escape.
    auto const buttons = OverlayCard {
        hAlign.right,
        vAlign.top,
        Margin{0, 64, 72, 0},
        StackPanel {
            translucent(Button {
                overlayMainLook,
                u"Сохранить",
                onClick = [this](Object const&, RoutedEventArgs&) { saveRequested(); },
            }),
            translucent(Button {
                overlayButtonLook,
                u"Выбрать другое изображение",
                onClick = [this](Object const&, RoutedEventArgs&) { chooseAnother(); },
            }),
            translucent(Button {
                overlayButtonLook,
                cancelFaceLook(),
                u"Выйти из мастера обложек",
                onClick = [this](Object const&, RoutedEventArgs&) { exitWizard(); },
            }),
        },
    };

    // Имя для чтеца экрана — слова подписи над полем: у поля ввода своего
    // слова на лице нет.
    Apply{nameBox_, width = 320.0, automationName = u"Название обложки"};

    // Диалог имени — в цветах обстановки, общих с панелью читалки (look.h):
    // он не бумага, а инструмент.
    Apply {
        namePanel_,
        hAlign.center,
        vAlign.center,
        visibility = Visibility::Collapsed,
        background = kChrome,
        borderBrush = kChromeEdge,
        BorderThickness{1},
        CornerRadius{6},
        Padding{20, 16},
        StackPanel {
            TextBlock {
                u"Название обложки",
                fontSize = 13,
                foreground = kChromeInk,
                Margin{0, 0, 0, 8},
            },
            nameBox_,
            StackPanel {
                Orientation::Horizontal,
                hAlign.right,
                Margin{0, 12, 0, 0},
                Button {
                    u"ОК",
                    Padding{18, 6},
                    Margin{0, 0, 8, 0},
                    onClick = [this](Object const&, RoutedEventArgs&) { finishNaming(true); },
                },
                Button {
                    u"Отмена",
                    Padding{18, 6},
                    onClick = [this](Object const&, RoutedEventArgs&) { finishNaming(false); },
                },
            },
        },
    };

    Apply {
        root_,
        isTabStop = true,
        visibility = Visibility::Collapsed,
        // Прозрачная, но настоящая кисть: без неё оверлей не участвует в
        // проверке попадания, и тянуть точки было бы не за что.
        background = colors.transparent,
        surfaceHost_,
        buttons,
        namePanel_,

        // Enter — действие по умолчанию: «Сохранить», а в открытом диалоге имени
        // — его «ОК». Escape — отмена: «Выйти из мастера обложек», а в диалоге —
        // его «Отмена». На пути вниз, чтобы клавиши работали при любом фокусе.
        onPreviewKeyDown =
            [this](Object const&, KeyRoutedEventArgs& args) {
                const bool naming = namePanel_.visibility() == Visibility::Visible;
                switch (args.key()) {
                    case VirtualKey::Enter:
                        if (naming) {
                            finishNaming(true);
                        } else {
                            saveRequested();
                        }
                        break;
                    case VirtualKey::Escape:
                        if (naming) {
                            finishNaming(false);
                        } else {
                            exitWizard();
                        }
                        break;
                    default: return;
                }
                args.handled(true);
            },

        onSizeChanged =
            [this](Object const&, SizeChangedEventArgs&) {
                // Координаты в долях, поэтому смена размеров ничего не двигает
                // по существу — точки остаются на своих местах снимка.
                if (resizeSurface()) redraw();
            },

        onPointerPressed =
            [this](Object const&, PointerRoutedEventArgs& args) {
                // Фокус — себе на каждом нажатии: щелчок по книге уводил его с
                // мастера, и Enter с Escape переставали работать.
                root_.focus(FocusState::Programmatic);

                const PointerPoint touch = args.getCurrentPoint(root_);
                if (!touch.properties().isLeftButtonPressed()) return;

                const std::optional<SkinEditor::Grip> grip =
                    editor_.gripAt(onGrid(touch.position()), area());
                if (!grip) return;

                dragged_ = grip;
                args.handled(true);
            },

        onPointerMoved =
            [this](Object const&, PointerRoutedEventArgs& args) {
                const SkinEditor::Point point = onGrid(args.getCurrentPoint(root_).position());

                // Куда встаёт точка — между соседками, корешок только по
                // вертикали, — решает модель правки; здесь только перерисовка.
                std::optional<SkinEditor::Grip> grip = dragged_;
                if (dragged_) {
                    editor_.drag(*dragged_, point, area());
                    redraw();
                    args.handled(true);
                } else {
                    grip = editor_.gripAt(point, area());
                }

                // Курсор — каждое движение заново: WinUI возвращает свою
                // стрелку, а задать курсор элементу проекция не умеет. Макрос
                // ресурса Windows допустим здесь — спрашиваем саму Windows.
                // Четыре стрелки у точки, которая ходит в обе оси, две — у
                // точки корешка.
                if (grip) {
                    ::SetCursor(::LoadCursorW(
                        nullptr, grip->point == kSpinePoint ? IDC_SIZENS : IDC_SIZEALL));
                }
            },

        onPointerReleased =
            [this](Object const&, PointerRoutedEventArgs& args) {
                if (!dragged_) return;
                dragged_.reset();
                args.handled(true);

                // Точку отпустили — кривые устоялись: время пересчитать карту
                // изгиба и показать страницу по-новому. Не на каждом движении:
                // пересборка карты стоит прохода по всем пикселям слоя.
                if (onCurvesChanged) onCurvesChanged();
            },
    };
}

void SkinWizard::openNew(std::filesystem::path image) {
    editor_.openNew(std::move(image));
    dragged_.reset();
    namePanel_.visibility(Visibility::Collapsed);

    redraw();
}

void SkinWizard::openEdit(const Skin& skin, std::filesystem::path image) {
    // Системная становится новой обложкой — это решает модель правки.
    editor_.openEdit(skin, std::move(image));
    dragged_.reset();
    namePanel_.visibility(Visibility::Collapsed);

    redraw();
}

void SkinWizard::show() {
    if (open_) return;
    open_ = true;
    root_.visibility(Visibility::Visible);
    root_.focus(FocusState::Programmatic);
}

void SkinWizard::hide() {
    if (!open_) return;
    open_ = false;
    dragged_.reset();
    namePanel_.visibility(Visibility::Collapsed);
    root_.visibility(Visibility::Collapsed);
}

bool SkinWizard::resizeSurface() {
    const auto width = static_cast<float>(root_.actualWidth());
    const auto height = static_cast<float>(root_.actualHeight());

    core::nullable<XamlRoot> const xamlRoot = root_.xamlRoot();
    float scale = xamlRoot ? static_cast<float>(xamlRoot->rasterizationScale()) : 1.0f;
    if (scale <= 0.0f) scale = 1.0f;

    if (width == width_ && height == height_ && scale == scale_ && !surface_.empty()) return false;

    width_ = width;
    height_ = height;
    scale_ = scale;

    const SizeInt32 pixels{static_cast<int32_t>(width * scale + 0.5f),
                           static_cast<int32_t>(height * scale + 0.5f)};
    if (pixels.width <= 0 || pixels.height <= 0) return false;

    if (surface_.empty()) {
        surface_.emplace_back(compositor_, pixels);
        visual_.brush(surface_[0].brush());
        ElementCompositionPreview::setElementChildVisual(surfaceHost_, visual_);
    } else {
        surface_[0].resize(pixels);
    }

    visual_.size({width, height});
    return true;
}

void SkinWizard::redraw() {
    if (surface_.empty() || width_ <= 0.0f || height_ <= 0.0f) return;

    surface_[0].draw([this](ID2D1DeviceContext* context) {
        // Поверхность в пикселях, рисование в DIP — как у полосы набора.
        D2D1_MATRIX_3X2_F atlas{};
        context->GetTransform(&atlas);
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_) *
                              *D2D1::Matrix3x2F::ReinterpretBaseType(&atlas));
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Прозрачный лист: снимок и страницу рисует полоса под оверлеем.
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> curveBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shade;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ring;
        context->CreateSolidColorBrush(kCurveColor, &curveBrush);
        context->CreateSolidColorBrush(kCurveShade, &shade);
        context->CreateSolidColorBrush(kGripFill, &fill);
        context->CreateSolidColorBrush(kGripRing, &ring);
        if (!curveBrush || !shade || !fill || !ring) return;

        // Смещение теневых линий — ровно пиксель экрана, какой бы ни был
        // масштаб: рисуем в DIP, а пиксель хотим физический.
        const float pixel = 1.0f / scale_;

        // Линии-подсказки: от верхней кривой к нижней, и только между крайними
        // точками — за ними кривая всё равно держит их значение, и линия во
        // всю ширину лишь мешала бы снимку. Корешок линия проходит насквозь:
        // точка там у обоих листов одна, и край в нём непрерывен.
        // Не top/bottom: это имена тегов DSL.
        const EdgeCurve& upper = editor_.skin().top;
        const EdgeCurve& lower = editor_.skin().bottom;
        const EdgeSpline upperEdge{upper};
        const EdgeSpline lowerEdge{lower};
        constexpr size_t last = static_cast<size_t>(EdgeCurve::kPoints) - 1;

        for (int row = 0; row < kGuideRows; ++row) {
            const float share = static_cast<float>(row) / (kGuideRows - 1);
            const float base = kEdgeInset + share * (1.0f - 2.0f * kEdgeInset);
            const float from = upper.x[0] + (lower.x[0] - upper.x[0]) * share;
            const float to = upper.x[last] + (lower.x[last] - upper.x[last]) * share;

            const int steps = std::max(2, static_cast<int>((to - from) * width_ / kCurveStep));
            D2D1_POINT_2F previous{};

            for (int step = 0; step <= steps; ++step) {
                const float u =
                    from + (to - from) * static_cast<float>(step) / static_cast<float>(steps);
                const float deviation = (upperEdge.at(u) - kEdgeInset) * (1.0f - share) +
                                        (lowerEdge.at(u) - (1.0f - kEdgeInset)) * share;
                const D2D1_POINT_2F point{u * width_, (base + deviation) * height_};

                // Линия рисуется тройкой: тёмная в пиксель выше, тёмная в
                // пиксель ниже и основная поверх — тень отбивает её и от
                // светлой бумаги, и от текста.
                if (step > 0) {
                    context->DrawLine({previous.x, previous.y - pixel},
                                      {point.x, point.y - pixel}, shade.Get(), 1.5f);
                    context->DrawLine({previous.x, previous.y + pixel},
                                      {point.x, point.y + pixel}, shade.Get(), 1.5f);
                    context->DrawLine(previous, point, curveBrush.Get(), 1.5f);
                }
                previous = point;
            }
        }

        for (const EdgeCurve* edited : {&upper, &lower}) {
            for (size_t at = 0; at < static_cast<size_t>(EdgeCurve::kPoints); ++at) {
                const D2D1_ELLIPSE circle{{edited->x[at] * width_, edited->y[at] * height_},
                                          kGripRadius, kGripRadius};
                context->FillEllipse(circle, fill.Get());
                context->DrawEllipse(circle, ring.Get(), 1.5f);
            }
        }
    });
}

void SkinWizard::chooseAnother() {
    if (onChooseAnother) onChooseAnother();
}

void SkinWizard::exitWizard() {
    if (onExit) onExit();
}

void SkinWizard::saveRequested() {
    // У правки старой обложки копия и имя уже есть — сохранение идёт сразу,
    // без диалога. Имя спрашивается только у новой.
    if (!editor_.needsName()) {
        if (onSave) onSave(editor_.skin(), editor_.imagePath());
        return;
    }
    beginNaming();
}

void SkinWizard::beginNaming() {
    nameBox_.text(editor_.skin().name);   // у правки — прежнее имя, у новой пусто
    namePanel_.visibility(Visibility::Visible);
    nameBox_.focus(FocusState::Programmatic);
}

void SkinWizard::finishNaming(bool save) {
    if (!save) {
        // «Отмена» — остаёмся в мастере, ничего не потеряв: точки как стояли,
        // так и стоят.
        namePanel_.visibility(Visibility::Collapsed);
        root_.focus(FocusState::Programmatic);
        return;
    }

    // Поле ещё не привязано к имени модели: текст кладётся туда здесь,
    // исправленным, — так, как положит его привязка.
    editor_.name.set(unicode::repaired(nameBox_.text()));
    std::optional<Skin> saved = editor_.result();
    if (!saved) return;   // безымянную сохранять некуда

    namePanel_.visibility(Visibility::Collapsed);
    if (onSave) onSave(std::move(*saved), editor_.imagePath());
}

}  // namespace bukvitsa::reader
