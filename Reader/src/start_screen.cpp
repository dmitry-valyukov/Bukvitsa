#include "start_screen.h"

#include <algorithm>
#include <chrono>

#include "Bind.h"
#include "look.h"

namespace bukvitsa::reader {

using namespace wxl;
using namespace std::chrono_literals;

namespace {

// Проявление: длительность одной кнопки и разбег между соседними. Четыре
// кнопки с шагом 40 мс успокаиваются к 320 мс — за 400 мс появление уже
// читается как задержка, так что расти этому некуда: если кнопок станет
// больше, уменьшать надо шаг, а не растягивать целое.
constexpr auto kFadeDuration = 220ms;
constexpr auto kStagger = 40ms;

// Большая кнопка, когда ей есть что продолжать: высота под обложку, обложка
// в пропорции витрины, автор — тем же приглушённым тоном, что и там.
constexpr double kContinueTall = 100.0;
constexpr double kCoverTall = 76.0;

// Откуда кнопка приезжает. Одной прозрачности мало — появление «из ничего»
// читается плоско, а десяток пикселей вверх делает его живым.
constexpr Vector3 kRiseFrom{0.0f, 14.0f, 0.0f};

// Разметка большой кнопки одна, с книгой и без: что в ней видно и какой она
// высоты, выводится из полей книги (`BindOutput`). Выводы — здесь, а не
// лямбдами в разметке: там открыты теги, и имя параметра вроде `title`
// спрятало бы одноимённый тег (C4459).

/// Есть книга — есть и строки названия и автора под надписью.
Visibility shownWithBook(u16_text const& bookTitle) {
    return bookTitle.empty() ? Visibility::Collapsed : Visibility::Visible;
}

/// Высота кнопки с книгой — под обложку. Без книги — та, что у пресета:
/// MinHeight берёт верх над Height, только когда больше его.
double heightWithBook(u16_text const& bookTitle) {
    return bookTitle.empty() ? 0.0 : kContinueTall;
}

/// Надпись без книги стоит посередине, как на всякой кнопке карточки; с
/// книгой — растянута: при выравнивании по краю содержимому отдали бы его
/// желанную ширину, и длинное название не обрезалось бы.
HorizontalAlignment alignedWithBook(u16_text const& bookTitle) {
    return bookTitle.empty() ? HorizontalAlignment::Center : HorizontalAlignment::Stretch;
}

/// Имя для чтеца экрана. Пока лицом кнопки была надпись, она же была и именем;
/// сетка с обложкой имени не даёт — имя ставится отдельно, из тех же слов, что
/// на кнопке.
u16_text saidWithBook(u16_text const& bookTitle) {
    u16_text said{u"Продолжить чтение"};
    if (bookTitle.empty()) return said;
    said += u": ";
    said += bookTitle;
    return said;
}

/// Обложка книги как источник картинки. Путь абсолютный, поэтому со схемой:
/// без неё wxl искал бы картинку рядом с исполняемым файлом — так же устроена
/// обложка на витрине.
ImageSource coverSource(std::filesystem::path const& cover) {
    if (cover.empty()) return {};
    std::u16string full = cover.u16string();
    std::replace(full.begin(), full.end(), u'\\', u'/');
    return ImageSource{u"file:///" + full};
}

Visibility shownWithCover(std::filesystem::path const& cover) {
    return cover.empty() ? Visibility::Collapsed : Visibility::Visible;
}

}  // namespace

// Визуал обёртки карточки берётся сразу, у ещё пустой обёртки: элемент
// отдаёт свой визуал и без содержимого, а прозрачность нужна раньше первого
// кадра.
StartScreen::StartScreen(const Compositor& compositor, Actions& actions)
    : actions_(actions),
      compositor_(compositor),
      cardVisual_(ElementCompositionPreview::getElementVisual(cardShell_)) {
    // Теги разметки — внутри строителей, не на уровне файла: там они накрыли
    // бы обычные слова (title, key, delay) и под /W4 каждое стало бы C4459.
    using namespace wxl::dsl;

    // Колонка текста большой кнопки: своя надпись, под ней название, под ним
    // автор. Grid со звёздной колонкой, а не горизонтальный StackPanel: тот
    // мерил бы текст бесконечной шириной, и длинному названию не с чего было
    // бы обрезаться. Без книги видна одна надпись — тем же кеглем и
    // начертанием, что у пресета кнопки.
    auto const lines = StackPanel {
        column = 1,
        vAlign.center,
        TextBlock{u"Продолжить чтение", fontSize = 19, FontWeight{600}},
        TextBlock {
            text = BindOutput{bookTitle_},
            visibility = BindOutput{bookTitle_, shownWithBook},
            fontSize = 13,
            Margin{0, 5, 0, 0},
            textTrimming.characterEllipsis,
        },
        TextBlock {
            text = BindOutput{bookAuthor_},
            visibility = BindOutput{bookTitle_, shownWithBook},
            fontSize = 12,
            Margin{0, 2, 0, 0},
            foreground = kDimInk,
            textTrimming.characterEllipsis,
        },
    };

    // Кнопки — на карточке поверх картинки, вид у них общий с мастером
    // обложек (look.h); своё у заставки — выравнивание и проявление. Каждая
    // отдаёт свой визуал в revealing_ (revealLater) до того, как дерево уедет
    // в конструктор Grid, и проступают они в том порядке, в каком встали.
    // Кнопка зовёт намерение прямо: method() отдаёт член интерфейса как
    // обработчик, корутина запускается и идёт сама.
    auto const panel = StackPanel {
        revealLater(Button {
            overlayMainLook,
            hAlign.stretch,
            minHeight = BindOutput{bookTitle_, heightWithBook},
            horizontalContentAlignment = BindOutput{bookTitle_, alignedWithBook},
            automationName = BindOutput{bookTitle_, saidWithBook},
            onClick = method(&actions_, &Actions::continueReading),
            content = Grid {
                columnDefinitions = u"auto,*",
                Image {
                    source = BindOutput{bookCover_, coverSource},
                    visibility = BindOutput{bookCover_, shownWithCover},
                    height = kCoverTall,
                    Margin{0, 0, 12, 0},
                },
                lines,
            },
        }),
        revealLater(Button {
            overlayButtonLook,
            hAlign.stretch,
            u"Моя библиотека",
            onClick = method(&actions_, &Actions::showLibrary),
        }),
        revealLater(Button {
            overlayButtonLook,
            hAlign.stretch,
            u"Добавить книгу",
            onClick = method(&actions_, &Actions::chooseBook),
        }),
        revealLater(Button {
            overlayButtonLook,
            hAlign.stretch,
            u"Добавить каталог",
            onClick = method(&actions_, &Actions::chooseFolder),
        }),
        revealLater(Button {
            overlayButtonLook,
            cancelFaceLook(),
            hAlign.stretch,
            u"Выйти из читалки",
            onClick = method(&actions_, &Actions::quit),
        }),
    };

    // Кнопки лежат на карточке — той же, что у мастера обложек. Проступать
    // ей вместе с ними, но гасить прозрачность самой карточки нельзя: у неё
    // заняты фасадные свойства (translation несёт тень), а трогать
    // handout-визуал элемента с фасадами запрещено — XAML тогда прикладывает
    // смещение карточки к попаданию мыши дважды, кнопки рисуются на месте, а
    // ловят щелчки за правым краем экрана (проверено UIA: x кнопок удвоился).
    // Поэтому прозрачностью проявляется обёртка, у которой фасадов нет.
    //
    // Сама карточка — библиотечная wxl::OverlayCard, та, что для страницы с
    // картинкой под ней. Своего здесь только место.
    Apply {
        cardShell_,
        OverlayCard {
            hAlign.right,
            vAlign.top,
            Margin{0, 64, 72, 0},
            panel,
        },
    };
    cardVisual_.opacity(0.0f);

    // Заставки в этом дереве нет. Задняя картинка окна ровно одна — задник
    // сцены, который ставит приложение (window.backgroundAsync); остров
    // прозрачен, и сквозь него видна она. Второй вывод той же картинки
    // XAML-элементом Image был бы дублем, а дублей быть не должно: остров
    // несёт только карточку с кнопками.
    Apply {
        root_,
        // Корень берёт фокус на себя, иначе клавиатура не работает вовсе:
        // событие клавиши начинается у того, на чём фокус, и пока фокуса нет
        // ни на чём, ловить нечего — ни на всплытии, ни на пути вниз.
        isTabStop = true,

        // Тема — светлая, явно: экран подобран под неё, а остров без своей
        // темы берёт тему приложений Windows, и вид заставки зависел бы от
        // настройки, о которой читалка не знает.
        requestedTheme = ElementTheme::Light,

        cardShell_,

        onLoaded = method(this, &StartScreen::loaded),

        // На пути вниз, чтобы клавиша работала независимо от того, на какой
        // кнопке стоит фокус.
        onPreviewKeyDown = method(this, &StartScreen::keyDown),
    };
}

void StartScreen::loaded(Grid const& self) {
    // Просить фокус раньше, чем дерево живо, бесполезно: элемент вне
    // визуального дерева тихо отказывает.
    self.focus(FocusState::Programmatic);
}

void StartScreen::keyDown(Object const&, KeyRoutedEventArgs& args) {
    // Enter — действие по умолчанию, то же, что большая кнопка; Escape —
    // отмена, то же, что «Выйти из читалки».
    switch (args.key()) {
        case VirtualKey::Enter:
            actions_.continueReading();
            break;
        case VirtualKey::Escape:
            actions_.quit();
            break;
        default: return;
    }
    args.handled(true);
}

Button StartScreen::revealLater(Button button) {
    // Подъём идёт по Translation, а НЕ по Offset. Offset — это то, чем XAML
    // расставляет элементы при разметке: анимация захватывает свойство себе,
    // и все четыре кнопки съезжаются в начало панели друг на друга. Проверено
    // на себе. Translation — отдельное свойство поверх разметки, и живёт оно
    // только после setIsTranslationEnabled, а до первой вставки в набор
    // свойств его вообще нет.
    ElementCompositionPreview::setIsTranslationEnabled(button, true);

    Visual visual = ElementCompositionPreview::getElementVisual(button);

    // Прозрачность и подъём ставятся здесь, а не в reveal(): между
    // построением дерева и готовностью приложения проходит кадр, и на нём
    // кнопка успела бы мигнуть во всю силу.
    //
    // Смещение тоже сразу — во время задержки анимация ещё не трогает
    // свойство, и кнопка со штатным смещением просто стояла бы на месте, а
    // потом прыгнула.
    visual.opacity(0.0f);
    visual.properties().insertVector3(L"Translation", kRiseFrom);
    revealing_.push_back(visual);

    return button;
}

void StartScreen::setContinueBook(u16_view bookTitle, u16_view bookAuthor,
                                  const std::filesystem::path& cover) {
    if (bookTitle.empty()) return;   // продолжать нечего — кнопка остаётся какой была

    // Разметку перестраивают привязки; одинаковое значение поле не объявляет,
    // так что тот же показ той же книги ничего не трогает. Название — последним:
    // от него зависят высота и видимость строк, и к его объявлению автор и
    // обложка уже на месте.
    bookAuthor_.set(u16_text{bookAuthor});
    bookCover_.set(cover);
    bookTitle_.set(u16_text{bookTitle});
}

void StartScreen::reveal() {
    if (revealed_) return;   // окно может стать видимым не один раз
    revealed_ = true;

    auto const easing = compositor_.createLinearEasingFunction();

    // Карточка — только прозрачностью и без разбега: подъём по Z несёт её
    // тень, и анимация Translation увела бы его в ноль.
    auto cardFade = compositor_.createScalarKeyFrameAnimation();
    cardFade.duration(kFadeDuration);
    cardFade.insertKeyFrame(1.0f, 1.0f, easing);
    cardVisual_.startAnimation(L"Opacity", cardFade);

    for (size_t index = 0; index < revealing_.size(); ++index) {
        auto const delay = kStagger * static_cast<int>(index);
        const Visual& visual = revealing_[index];

        auto fade = compositor_.createScalarKeyFrameAnimation();
        fade.duration(kFadeDuration);
        fade.delayTime(delay);
        fade.insertKeyFrame(1.0f, kOverlayButtonOpacity, easing);

        auto rise = compositor_.createVector3KeyFrameAnimation();
        rise.duration(kFadeDuration);
        rise.delayTime(delay);
        rise.insertKeyFrame(1.0f, Vector3{0.0f, 0.0f, 0.0f}, easing);

        // Анимация крутится на потоке DWM: пока кнопки проступают, поток
        // приложения свободен — и будет чем занять его, когда за кнопками
        // появится реестр книг.
        visual.startAnimation(L"Opacity", fade);
        visual.startAnimation(L"Translation", rise);
    }
}

}  // namespace bukvitsa::reader
