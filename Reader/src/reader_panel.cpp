#include <algorithm>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "reader_panel.h"

#include "bukvitsa/reader/book_index.h"

#include "Bind.h"
#include "look.h"

import wxl.fmt;

namespace bukvitsa::reader {

using namespace wxl;
using namespace std::chrono_literals;

namespace {

// Обстановка ящиков — общая с мастером обложек (look.h); своё у панели —
// тихий тон подписей и отметка того, что выбрано сейчас. wxl:: — потому что
// rgb() темы страницы (theme.h) даёт цвет Direct2D и здесь заслонил бы этот.
constexpr Color kChromeDim = wxl::rgb(154, 150, 142);
constexpr Color kChromeActive = wxl::rgba(255, 255, 255, 0.132);

constexpr double kWidth = 380;

// Выезд: короткий, потому что панель открывают между двумя строчками текста и
// ждать её не должны. Двести миллисекунд — предел, за которым появление
// читается как задержка.
constexpr auto kSlide = 180ms;

// Вид панели. Теги — с квалификатором `dsl::`: открытые на уровне файла, они
// спрятали бы обычные слова (C4459); у константы нет строителя, где их открыть.

/// Ящик у левой или правой стенки окна: тёмный, во всю высоту, с кромкой со
/// стороны страницы; щелчки и колесо по себе гасит.
auto drawerLook(HorizontalAlignment side) {
    using namespace wxl::dsl;

    // Кромка у ящика одна — та, что смотрит на страницу. Отбивку от стенок
    // правому ящику даёт сам ящик; у левого её несут его ряды.
    // Не left: это имя тега DSL, и локальная переменная его прятала бы.
    const bool onLeft = side == HorizontalAlignment::Left;
    const double pad = onLeft ? 0.0 : 12.0;

    return Preset {
        horizontalAlignment = side,
        vAlign.stretch,
        width = kWidth,
        background = kChrome,
        borderBrush = kChromeEdge,
        BorderThickness{onLeft ? 0.0 : 1.0, 0.0, onLeft ? 1.0 : 0.0, 0.0},
        Padding{pad, pad},

        // Ящик — не страница: щелчок и колесо по его пустому месту здесь и
        // кончаются, иначе они всплыли бы к полосе, и та листала бы книгу под
        // открытой панелью и закрывала её правой кнопкой.
        onPointerPressed = [](Object const&, PointerRoutedEventArgs& args) { args.handled(true); },
        onPointerWheelChanged = [](Object const&, PointerRoutedEventArgs& args) {
            args.handled(true);
        },
    };
}

/// Кромка и скругление кнопки ящика; своей заливки нет — под кнопкой ящик.
constexpr auto drawerFrameLook = Preset {
    dsl::background = colors.transparent,
    dsl::borderBrush = kChromeEdge,
    BorderThickness{1},
    CornerRadius{4},
};

/// Кнопка ящика светлыми чернилами — выход на полку, вкладки, закладка,
/// темы и обложки.
constexpr auto drawerButtonLook = Preset{dsl::foreground = kChromeInk, drawerFrameLook};

/// Кнопка ящика тише — то, что рядом с главным: глифы у обложек и
/// «Добавить обложку…».
constexpr auto drawerQuietLook = Preset{dsl::foreground = kChromeDim, drawerFrameLook};

/// Выбор темы или обложки в правом ящике. Выбранную отмечает markTheme() — тем
/// же цветом, что открытую вкладку.
constexpr auto themeChipLook = Preset{drawerButtonLook, dsl::fontSize = 13, Padding{12, 6}};

/// Кнопка-глиф рядом с обложкой — шестерёнка и корзина. Их две, и обе
/// одинаковы во всём, кроме глифа, подсказки и того, что делают. Слова на
/// лице у неё нет, а кнопка без текста обязана иметь тултип (правило
/// дизайна) — его ставит употребление, и он называет конкретную обложку, а
/// не действие вообще. Не constexpr: имя шрифта — строка, которую надо
/// завести.
auto const glyphButtonLook = Preset {
    drawerQuietLook,
    dsl::fontFamily = FontFamily{u"Segoe Fluent Icons"},
    dsl::fontSize = 13,
    Padding{8, 6},
};

/// Подпись над группой настроек — «Тема», «Обложки», «Кегль»…: тише текста и
/// с отбивкой сверху.
constexpr auto groupCaptionLook = Preset {
    dsl::fontSize = 13,
    dsl::foreground = kChromeDim,
    Margin{0, 12, 0, 2},
};

/// Пояснение над списком вкладки — «Нашлось: 3», «Закладок пока нет.»: тихое
/// и с переносом.
constexpr auto drawerNoteLook = Preset {
    dsl::fontSize = 13,
    dsl::foreground = kChromeDim,
    dsl::textWrapping.wrap,
};

}  // namespace

// Отметка темы идёт за полем полосы, а не за нажатием здешней кнопки: тему
// меняют и клавишей T мимо панели. Слушатель ставится в списке инициализации:
// cookie_t не присваивается, а срабатывает он только на смену поля — не раньше,
// чем дерево панели собрано.
//
// Контролы панели — поля, построенные вместе с ней; buildTree() их одевает и
// складывает в дерево. Визуалы ящиков — здесь же, от самих ящиков: элемент
// отдаёт свой визуал и пустым.
ReaderPanel::ReaderPanel(const Compositor& compositor, BookView& view, Settings& settings)
    : compositor_(compositor),
      view_(view),
      prefs_(settings),
      themeWatch_(view.theme.on_change([this](int) noexcept { markTheme(); })),
      navigationVisual_(slidingVisual(navigation_, -static_cast<float>(kWidth))),
      settingsVisual_(slidingVisual(settings_, static_cast<float>(kWidth))),
      linear_(compositor_.createLinearEasingFunction()) {
    buildTree();
}

ReaderPanel::~ReaderPanel() {
    view_.theme.remove_change(themeWatch_);
}

void ReaderPanel::buildTree() {
    using namespace wxl::dsl;

    tabPages_ = {buildContents(), buildSearch(), buildBookmarks()};

    // Выход на полку — первым, отдельной строкой над вкладками. Вкладки
    // говорят о книге, которая открыта; эта кнопка — о том, чтобы открыть
    // другую, и стоять в одном ряду с ними ей не за что.
    auto const shelf = Button {
        drawerButtonLook,
        row = 0,
        u"←  Моя библиотека",
        hAlign.stretch,
        horizontalContentAlignment = HorizontalAlignment::Left,
        fontSize = 14,
        Margin{12, 12, 12, 0},
        Padding{12, 8},
        onClick =
            [this](Object const&, RoutedEventArgs&) {
                if (onLibrary) onLibrary();
            },
    };

    // Вкладки. Открытую отмечает showTab() — заливкой, по кнопкам из
    // tabButtons_ тем же счётом, что у страниц.
    auto const strip = StackPanel {
        row = 1,
        Orientation::Horizontal,
        Margin{12, 12, 12, 6},
    };
    for (auto&& [said, tab] : {std::pair{u"Оглавление", Tab::Contents},
                                  std::pair{u"Поиск", Tab::Search},
                                  std::pair{u"Закладки", Tab::Bookmarks}}) {
        auto const button = Button {
            drawerButtonLook,
            said,
            fontSize = 14,
            Margin{0, 0, 6, 0},
            Padding{12, 6},
            onClick = [this, tab = tab](Object const&, RoutedEventArgs&) { open(tab); },
        };
        tabButtons_.push_back(button);
        strip.children().append(button);
    }

    Apply{pages_, row = 2, Margin{12, 6, 12, 12}};
    for (const UIElement& page : tabPages_) {
        pages_.children().append(page);
    }

    Apply {
        navigation_,
        drawerLook(HorizontalAlignment::Left),
        child = Grid {
            rowDefinitions = u"auto,auto,*",
            shelf,
            strip,
            pages_,
        },
    };
    Apply{settings_, drawerLook(HorizontalAlignment::Right), child = buildSettings()};

    // Оба ящика — в одном холсте с прозрачной, но настоящей кистью: без неё
    // холст не участвует в проверке попадания, а с ней ловит щелчок мимо
    // ящиков и закрывает оба, съедая щелчок, — как свет-дисмисс у Flyout.
    // Сам Flyout не подошёл: попапов со свет-дисмиссом у WinUI в один момент
    // один, второй закрывает первый. Пока панель закрыта, холст свёрнут, и
    // страница между ящиками остаётся страницей.
    Apply {
        root_,
        visibility = Visibility::Collapsed,
        background = colors.transparent,
        navigation_,
        settings_,
        onPointerPressed =
            [this](Object const&, PointerRoutedEventArgs& args) {
                // Сюда доходят только щелчки по самому холсту: щелчки по ящикам
                // они же и гасят.
                close();
                args.handled(true);
            },
    };

    showTab(Tab::Contents);
}

Visual ReaderPanel::slidingVisual(const UIElement& box, float offscreen) {
    // Выезд идёт по Translation, а не по Offset: Offset — это то, чем XAML
    // расставляет элементы при разметке, и анимация его отобрала бы.
    ElementCompositionPreview::setIsTranslationEnabled(box, true);
    Visual visual = ElementCompositionPreview::getElementVisual(box);
    visual.properties().insertVector3(L"Translation", Vector3{offscreen, 0.0f, 0.0f});
    return visual;
}

void ReaderPanel::slide(Visual& visual, float x) {
    auto animation = compositor_.createVector3KeyFrameAnimation();
    animation.duration(kSlide);
    animation.insertKeyFrame(1.0f, Vector3{x, 0.0f, 0.0f}, linear_);
    visual.startAnimation(L"Translation", animation);
}

Button ReaderPanel::listItem(zstring_view said, zstring_view under, float indent,
                             std::function<void()> action) {
    using namespace wxl::dsl;

    auto const lines = StackPanel {
        TextBlock {
            said,
            fontSize = 14,
            foreground = kChromeInk,
            twoLinesLook,
        },
    };

    if (!under.empty()) {
        lines.children().append(TextBlock {
            under,
            fontSize = 12,
            foreground = kChromeDim,
            Margin{0, 2, 0, 0},
            oneLineLook,
        });
    }

    return Button {
        // Лицо строки — панель из надписей, а не слово, и чтецу экрана кнопка с
        // такой начинкой безымянна: имя кнопки берётся из содержимого, только
        // когда оно строка. Подсказки при наведении строке не нужно — её слова
        // и так на экране, — поэтому имя отдельно, не toolTip.
        automationName = said,
        hAlign.stretch,
        horizontalContentAlignment = HorizontalAlignment::Stretch,
        Margin{indent, 1, 0, 1},
        Padding{8, 6},
        background = colors.transparent,
        borderBrush = colors.transparent,
        BorderThickness{0},
        CornerRadius{4},
        onClick = [action](Object const&, RoutedEventArgs&) { if (action) action(); },
        content = lines,
    };
}

/* ---------------- вкладки ---------------- */

UIElement ReaderPanel::buildContents() {
    using namespace wxl::dsl;

    return ScrollViewer {
        horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
        content = contentsList_,
    };
}

UIElement ReaderPanel::buildSearch() {
    using namespace wxl::dsl;

    Apply {
        searchBox_,
        row = 0,
        // Подсказка в пустом поле — не имя: чтецу экрана поле зовётся отдельно.
        automationName = u"Поиск по книге",
        placeholderText = u"Что искать",
        Margin{0, 0, 0, 8},

        // Поиск по Enter, а не по каждой букве: искать по одной букве в романе —
        // это тысячи находок, из которых читателю не нужна ни одна.
        onKeyDown =
            [this](Object const&, KeyRoutedEventArgs& args) {
                if (args.key() != VirtualKey::Enter) return;
                runSearch();
                args.handled(true);
            },
    };

    Apply{searchNote_, drawerNoteLook, row = 1, u"Введите слово и нажмите Enter."};

    return Grid {
        rowDefinitions = u"auto,auto,*",
        searchBox_,
        searchNote_,
        ScrollViewer {
            row = 2,
            horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
            content = searchList_,
        },
    };
}

UIElement ReaderPanel::buildBookmarks() {
    using namespace wxl::dsl;

    Apply{bookmarkNote_, drawerNoteLook, row = 1};

    return Grid {
        rowDefinitions = u"auto,auto,*",
        Button {
            drawerButtonLook,
            row = 0,
            u"Заложить эту страницу",
            hAlign.stretch,
            Margin{0, 0, 0, 8},
            background = kChromeActive,
            onClick = [this](Object const&, RoutedEventArgs&) { toggleBookmark(); },
        },
        bookmarkNote_,
        ScrollViewer {
            row = 2,
            horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
            content = bookmarkList_,
        },
    };
}

UIElement ReaderPanel::buildSettings() {
    using namespace wxl::dsl;

    refreshThemes();

    // Ползунок, а не пара кнопок: кегль подбирают, а не выставляют числом, и
    // видеть весь ход сразу удобнее, чем нажимать «плюс» восемь раз.
    //
    // Привязан к полю настроек в обе стороны: показывает поле и пишет в него.
    // В то же поле пишут колесо и клавиши полосы — и ползунок идёт за ними
    // сам, без обработчика и без флага «это мы сами его двигаем». Единицы у
    // ползунка и поля одни (проценты у интерлиньяжа и полей, см. Settings).
    //
    // Подпись над ползунком — его же имя для чтеца экрана: у ползунка нет
    // слова на лице, и без имени Narrator называет его просто ползунком.
    // Одни слова на подпись и имя, поэтому группа собирается здесь целиком.
    auto setting = [](zstring_view said, double low, double high, double step,
                      observable<double>& field) {
        return StackPanel {
            TextBlock{groupCaptionLook, said},
            Slider {
                automationName = said,
                minimum = low,
                maximum = high,
                stepFrequency = step,
                value = Bind{field},
                Margin{0, 0, 0, 4},
            },
        };
    };

    return ScrollViewer {
        horizontalScrollBarVisibility = ScrollBarVisibility::Disabled,
        content = StackPanel {
            // Подписи «Тема» и «Обложки» ставит сама полоса набора: она знает,
            // где кончается одна группа и начинается другая, а собирается
            // заново при каждой смене реестра.
            themesPanel_,
            setting(u"Кегль", kFontSizeMin, kFontSizeMax, kFontSizeStep, prefs_.fontSize),
            setting(u"Интерлиньяж", kLineHeightMin, kLineHeightMax, kLineHeightStep,
                    prefs_.lineHeight),
            setting(u"Поля", kMarginMin, kMarginMax, kMarginStep, prefs_.margin),
            TextBlock {
                u"Кегль меняется ещё и Ctrl с колесом, а тема — клавишей T.",
                fontSize = 12,
                foreground = kChromeDim,
                Margin{0, 16, 0, 0},
                textWrapping.wrap,
            },
        },
    };
}

/* ---------------- показ ---------------- */

void ReaderPanel::showTab(Tab tab) {
    tab_ = tab;
    for (size_t i = 0; i < tabPages_.size(); ++i) {
        tabPages_[i].visibility(static_cast<size_t>(tab) == i ? Visibility::Visible
                                                                  : Visibility::Collapsed);
    }
    for (size_t i = 0; i < tabButtons_.size(); ++i) {
        tabButtons_[i].background(
            SolidColorBrush{static_cast<size_t>(tab) == i ? kChromeActive : colors.transparent});
    }
}

void ReaderPanel::open(Tab tab) {
    showTab(tab);

    switch (tab) {
        case Tab::Contents: fillContents(); break;
        case Tab::Bookmarks: fillBookmarks(); break;
        case Tab::Search: break;   // список остаётся от прошлого поиска
    }

    show();

    if (tab == Tab::Search) searchBox_.focus(FocusState::Programmatic);
}

void ReaderPanel::toggle() {
    if (open_) {
        close();
    } else {
        open(tab_);
    }
}

void ReaderPanel::show() {
    if (open_) return;
    open_ = true;

    root_.visibility(Visibility::Visible);
    slide(navigationVisual_, 0.0f);
    slide(settingsVisual_, 0.0f);
}

void ReaderPanel::close() {
    if (!open_) return;
    open_ = false;

    // Уехавшие ящики надо ещё и спрятать вместе с холстом, иначе они
    // продолжат ловить щелчки: ящики за краями экрана, холст — на странице.
    // Конец анимации узнаётся пакетом, а не таймером: пакет сам скажет, когда
    // последняя из двух анимаций в нём закончилась.
    auto batch = compositor_.createScopedBatch(CompositionBatchTypes::Animation);

    slide(navigationVisual_, -static_cast<float>(kWidth));
    slide(settingsVisual_, static_cast<float>(kWidth));

    batch.add_onCompleted([this](Object const&, CompositionBatchCompletedEventArgs&) {
        if (!open_) root_.visibility(Visibility::Collapsed);
    });
    batch.end();

    // Фокус — обратно полосе: он мог остаться на ползунке или кнопке ящика, а
    // уехавший и спрятанный элемент клавиш не получает — полоса бы оглохла.
    view_.root().focus(FocusState::Programmatic);
}

void ReaderPanel::setState(BookState* state) {
    state_ = state;
    if (tab_ == Tab::Bookmarks) fillBookmarks();
}

/* ---------------- содержимое вкладок ---------------- */

void ReaderPanel::fillContents() {
    contentsList_.children().clear();

    const sta_vector<ContentsEntry> contents = contentsOf(view_.blocks());
    if (contents.empty()) {
        contentsList_.children().append(listItem(u"В этой книге нет заголовков", {}, 0, {}));
        return;
    }

    for (const ContentsEntry& entry : contents) {
        const uint32_t offset = entry.charOffset;
        contentsList_.children().append(
            // Заголовок оглавления — вид в текст блока, без нуля за ним: в разметку
            // он идёт строкой WinRT, которую мы и делаем сами.
            listItem(hstring{entry.title}, {}, std::min<float>(entry.level, 4) * 14.0f,
                     [this, offset] { view_.goToCharOffset(offset); }));
    }
}

void ReaderPanel::fillBookmarks() {
    bookmarkList_.children().clear();

    if (!state_ || state_->bookmarks.empty()) {
        bookmarkNote_.text(u"Закладок пока нет.");
        return;
    }

    bookmarkNote_.text({});

    for (const Bookmark& mark : state_->bookmarks) {
        const uint32_t offset = mark.charOffset;
        bookmarkList_.children().append(
            listItem(mark.hint.empty() ? zstring_view{u"Закладка"} : zstring_view{mark.hint}, {}, 0,
                     [this, offset] { view_.goToCharOffset(offset); }));
    }
}

void ReaderPanel::runSearch() {
    searchList_.children().clear();

    // Текст поля — чужой: пришёл из контрола строкой WinRT и в поиск идёт
    // проверенным (`searchQuery`). Строка держится, пока жив вид на неё.
    const hstring typed = searchBox_.text();
    const std::optional<u16_view> needle = searchQuery(std::u16string_view(typed));
    if (!needle) {
        searchNote_.text(u"Введите слово и нажмите Enter.");
        return;
    }

    const sta_vector<SearchHit> hits = searchBook(view_.blocks(), *needle);
    if (hits.empty()) {
        searchNote_.text(core::format(u"«{}» в книге не нашлось.", *needle));
        return;
    }

    searchNote_.text(core::format(u"Нашлось: {}", hits.size()));

    for (const SearchHit& hit : hits) {
        const uint32_t offset = hit.charOffset;
        searchList_.children().append(
            listItem(hit.context, {}, 0, [this, offset] { view_.goToCharOffset(offset); }));
    }
}

void ReaderPanel::toggleBookmark() {
    if (!state_ || !view_.isOpen()) return;

    // Поставить или снять и где ей лежать — решает состояние книги; панель
    // только перерисовывает список.
    const uint32_t here = view_.readingPosition();
    state_->toggleBookmark(here, hintAt(view_.blocks(), here));

    fillBookmarks();
    if (onStateChanged) onStateChanged();
}

void ReaderPanel::refreshThemes() {
    using namespace wxl::dsl;

    themesPanel_.children().clear();
    themeButtons_.clear();

    // Тема — это ровный цвет бумаги, и таких три. Они коротки и помещаются в
    // строчку; фотография среди них не стоит больше — снимок носит обложка.
    //
    // Индексы тем сквозные: сперва встроенные, затем обложки — ровно так их
    // считает и полоса набора. markTheme() ходит по кнопкам тем же счётом.
    themesPanel_.children().append(TextBlock{groupCaptionLook, u"Тема"});

    auto const builtins = StackPanel{Orientation::Horizontal};
    for (int index = 0; index < kThemeCount; ++index) {
        auto const button = Button {
            themeChipLook,
            kThemes[index].name,
            Margin{0, 0, 6, 0},
            onClick = [this, index](Object const&, RoutedEventArgs&) { view_.setTheme(index); },
        };
        themeButtons_.push_back(button);
        builtins.children().append(button);
    }
    themesPanel_.children().append(builtins);

    // Обложки — по строке на каждую: имя даёт читатель, и в строчку они не
    // помещаются. Сперва системные, приехавшие с программой, потом заведённые
    // читателем; порядок задаёт не панель, а сам список (BookView::setSkins).
    //
    // Рядом с каждой шестерёнка: обложку не только выбирают, но и правят, и
    // дорога к правке стоит у самой обложки. Корзина — только у своей:
    // системной в реестре нет, удалять нечего, и кнопка вела бы к тому, чего
    // не бывает.
    //
    // Спросить «точно ли» панель не может и не должна: окна у неё нет, а
    // удаление необратимо — вопрос задаёт приложение, которому принадлежат и
    // окно, и реестр.
    themesPanel_.children().append(TextBlock{groupCaptionLook, u"Обложки"});

    const std::vector<Skin>& skins = view_.skins();
    for (size_t index = 0; index < skins.size(); ++index) {
        const int themeIndex = kThemeCount + static_cast<int>(index);
        auto const button = Button {
            themeChipLook,
            skins[index].name,
            Margin{0, 6, 0, 0},
            onClick =
                [this, themeIndex](Object const&, RoutedEventArgs&) { view_.setTheme(themeIndex); },
        };
        themeButtons_.push_back(button);

        const u16_text skinName = skins[index].name;

        auto const line = StackPanel {
            Orientation::Horizontal,
            button,
            Button {
                glyphButtonLook,
                u"",   // шестерёнка Segoe Fluent Icons
                toolTip = core::format(u"Настроить подложку «{}»", skinName),
                Margin{6, 6, 0, 0},
                onClick =
                    [this, skinName](Object const&, RoutedEventArgs&) {
                        if (onEditSkin) onEditSkin(skinName);
                    },
            },
        };

        if (!skins[index].system) {
            line.children().append(Button {
                glyphButtonLook,
                u"",   // корзина оттуда же
                toolTip = core::format(u"Удалить обложку «{}»", skinName),
                Margin{6, 6, 0, 0},
                onClick =
                    [this, skinName](Object const&, RoutedEventArgs&) {
                        if (onDeleteSkin) onDeleteSkin(skinName);
                    },
            });
        }

        themesPanel_.children().append(line);
    }

    // Дорога в мастер — последней строкой, после всех тем.
    themesPanel_.children().append(Button {
        drawerQuietLook,
        u"Добавить обложку…",
        fontSize = 13,
        Margin{0, 6, 0, 0},
        Padding{12, 6},
        onClick =
            [this](Object const&, RoutedEventArgs&) {
                if (onAddSkin) onAddSkin();
            },
    });

    markTheme();
}

void ReaderPanel::markTheme() {
    // Тема — выбор из трёх, а выбор видно только тогда, когда выбранное
    // отмечено. Отмечается тем же цветом, что и открытая вкладка: одна
    // и та же мысль — «вот это сейчас».
    for (int index = 0; index < static_cast<int>(themeButtons_.size()); ++index) {
        themeButtons_[static_cast<size_t>(index)].background(
            SolidColorBrush{index == view_.theme.get() ? kChromeActive : colors.transparent});
    }
}

}  // namespace bukvitsa::reader
