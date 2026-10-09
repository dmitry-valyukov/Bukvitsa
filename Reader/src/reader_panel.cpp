#include <algorithm>

// Заголовки проекта после всех стандартных: они ведут к импорту модуля книги,
// а стандартный заголовок после импорта MSVC уже не принимает. Свой первым:
// он единственный тянет за собой стандартные заголовки, которых нет здесь.
#include "reader_panel.h"

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

/// Кнопка ящика светлыми чернилами — выход на полку, вкладки, закладка.
constexpr auto drawerButtonLook = Preset{dsl::foreground = kChromeInk, drawerFrameLook};

/// Кнопка ящика тише — то, что рядом с главным: глифы у обложек и
/// «Добавить обложку…».
constexpr auto drawerQuietLook = Preset{dsl::foreground = kChromeDim, drawerFrameLook};

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

/// Строка списка вкладки — оглавления, находок, закладок — как её контейнер:
/// без своей высоты, с отбивкой под надпись, надпись во всю ширину. Наведение
/// и нажатие подсвечивает сам контейнер.
constexpr auto tabRowLook = Preset {
    dsl::minHeight = 0.0,
    Padding{8, 6},
    Margin{0, 1},
    dsl::horizontalContentAlignment = HorizontalAlignment::Stretch,
};

/// Список вкладки: строка ведёт к месту в книге щелчком, выбора нет —
/// переход не оставляет за собой отмеченной строки.
constexpr auto tabListLook = Preset {
    dsl::selectionMode = ListViewSelectionMode::None,
    dsl::isItemClickEnabled = true,
    dsl::itemContainerStyle = tabRowLook,
};

/// Надпись строки списка вкладки: светлыми чернилами, не длиннее двух строк.
constexpr auto tabRowTextLook = Preset{dsl::fontSize = 14, dsl::foreground = kChromeInk, twoLinesLook};

/// Строка списка тем как её контейнер: одна высота у темы и у обложки с её
/// кнопками; выбранную отмечает сам контейнер.
constexpr auto themeRowLook = Preset {
    dsl::minHeight = 36.0,
    Padding{12, 2},
    Margin{0, 1},
    dsl::horizontalContentAlignment = HorizontalAlignment::Stretch,
};

/// Пояснение над пустым списком — «Закладок пока нет.», «В этой книге нет
/// заголовков»: видно, пока строк нет.
Visibility shownIfNone(uint32_t count) {
    return count == 0 ? Visibility::Visible : Visibility::Collapsed;
}

}  // namespace

// Выбор в списке тем идёт за полем полосы, а не за нажатием здешней строки:
// тему меняют и клавишей T мимо панели. Списки вкладок привязаны к спискам
// моделей — оглавлению и закладкам открытой книги, находкам поиска: их меняет
// не панель. Слушатели чужих полей ставятся в списке инициализации: cookie_t
// не присваивается, а срабатывают они только на смену поля — не раньше, чем
// дерево панели собрано.
//
// Контролы панели — поля, построенные вместе с ней; buildTree() их одевает и
// складывает в дерево. Визуалы ящиков — здесь же, от самих ящиков: элемент
// отдаёт свой визуал и пустым.
ReaderPanel::ReaderPanel(const Compositor& compositor, BookView& view, Settings& settings,
                         BookPlaces& places, ThemeList& themes, BookSearch& search,
                         Actions& actions)
    : actions_(actions),
      compositor_(compositor),
      view_(view),
      prefs_(settings),
      places_(places),
      themes_(themes),
      search_(search),
      themeWatch_(view.theme.on_change(method(this, &ReaderPanel::themeChanged))),
      choicesWatch_(themes.choices().on_change(method(this, &ReaderPanel::choicesChanged))),
      chosen_{selectionOf(view.theme.get())},
      navigationVisual_(slidingVisual(navigation_, -static_cast<float>(kWidth))),
      settingsVisual_(slidingVisual(settings_, static_cast<float>(kWidth))),
      linear_(compositor_.createLinearEasingFunction()) {
    // Своё поле — свой слушатель: уходит вместе с полем.
    static_cast<void>(chosen_.on_change(method(this, &ReaderPanel::themeChosen)));
    buildTree();
}

ReaderPanel::~ReaderPanel() {
    themes_.choices().remove_change(choicesWatch_);
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
        onClick = method(&actions_, &Actions::showLibrary),
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
        onPointerPressed = method(this, &ReaderPanel::canvasPressed),
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

void ReaderPanel::canvasPressed(Object const&, PointerRoutedEventArgs& args) {
    // Сюда доходят только щелчки по самому холсту: щелчки по ящикам они же и
    // гасят.
    close();
    args.handled(true);
}

/* ---------------- вкладки ---------------- */

UIElement ReaderPanel::buildContents() {
    using namespace wxl::dsl;

    // Оглавление — список, привязанный к заголовкам открытой книги: новая
    // книга заменяет его одним сбросом, и строятся только строки на экране.
    // Отступ строки — по уровню заголовка, глубже четвёртого не уходит.
    return Grid {
        TextBlock {
            drawerNoteLook,
            u"В этой книге нет заголовков",
            visibility = BindOutput{places_.contents().count(), shownIfNone},
        },
        ListView {
            tabListLook,
            itemsSource = BindOutput {places_.contents(), [](ContentsEntry const& heading) {
                // Заголовок оглавления — вид в текст блока, без нуля за ним: в
                // разметку он идёт строкой WinRT, которую мы и делаем сами.
                // Те же слова — имя строки для чтеца экрана.
                hstring const said{heading.title};
                return TextBlock {
                    tabRowTextLook,
                    said,
                    automationName = said,
                    Margin{std::min<float>(heading.level, 4) * 14.0f, 0, 0, 0},
                };
            }},
            onItemClick = method(this, &ReaderPanel::headingClicked),
        },
    };
}

UIElement ReaderPanel::buildSearch() {
    using namespace wxl::dsl;

    // Поле привязано к запросу модели в обе стороны и кладёт его уже
    // проверенным; подпись под ним — к её подписи. Сам поиск — по Enter.
    Apply {
        searchBox_,
        row = 0,
        // Подсказка в пустом поле — не имя: чтецу экрана поле зовётся отдельно.
        automationName = u"Поиск по книге",
        placeholderText = u"Что искать",
        Margin{0, 0, 0, 8},
        text = Bind{search_.query},
        onKeyDown = method(this, &ReaderPanel::searchKeyDown),
    };

    // Находки — список, привязанный к находкам модели: новый поиск — один
    // сброс, и строятся только строки на экране, а не все двести; Enter и
    // стрелки по строкам даёт сам список.
    return Grid {
        rowDefinitions = u"auto,auto,*",
        searchBox_,
        TextBlock{drawerNoteLook, row = 1, text = BindOutput{search_.status()}},
        ListView {
            tabListLook,
            row = 2,
            itemsSource = BindOutput {search_.hits(), [](SearchHit const& hit) {
                return TextBlock{tabRowTextLook, hit.context, automationName = hit.context};
            }},
            onItemClick = method(this, &ReaderPanel::hitClicked),
        },
    };
}

UIElement ReaderPanel::buildBookmarks() {
    using namespace wxl::dsl;

    return Grid {
        rowDefinitions = u"auto,auto,*",
        Button {
            drawerButtonLook,
            row = 0,
            u"Заложить эту страницу",
            hAlign.stretch,
            Margin{0, 0, 0, 8},
            background = kChromeActive,
            onClick = method(&actions_, &Actions::toggleBookmark),
        },
        TextBlock {
            drawerNoteLook,
            row = 1,
            u"Закладок пока нет.",
            visibility = BindOutput{places_.bookmarks().count(), shownIfNone},
        },
        // Закладки — список, привязанный к закладкам открытой книги:
        // поставленная встаёт одной строкой на своё место, снятая уходит одна.
        ListView {
            tabListLook,
            row = 2,
            itemsSource = BindOutput {places_.bookmarks(), [](Bookmark const& mark) {
                zstring_view const said = mark.hint.empty() ? zstring_view{u"Закладка"} : zstring_view{mark.hint};
                return TextBlock{tabRowTextLook, said, automationName = said};
            }},
            onItemClick = method(this, &ReaderPanel::bookmarkClicked),
        },
    };
}

UIElement ReaderPanel::buildSettings() {
    using namespace wxl::dsl;

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
            TextBlock{groupCaptionLook, u"Тема"},

            // Темы и обложки — один список с родным выбором: строка `n` —
            // тема `n` (ThemeList), выбранную отмечает сам список, и её же
            // читает экранный диктор. Сперва три ровных цвета бумаги, за ними
            // обложки — системные, приехавшие с программой, потом заведённые
            // читателем; порядок задаёт не панель, а список тем. Сохранение
            // или удаление обложки собирает строки заново, а не панель.
            //
            // Рядом с обложкой — шестерёнка: обложку не только выбирают, но и
            // правят, и дорога к правке стоит у самой обложки. Корзина — только
            // у своей: системной в реестре нет, удалять нечего, и кнопка вела
            // бы к тому, чего не бывает. Спросить «точно ли» панель не может и
            // не должна: окна у неё нет, а удаление необратимо — вопрос задаёт
            // приложение, которому принадлежат и окно, и реестр.
            //
            // Выбор — своим полем, а не темой полосы (chosen_).
            ListView {
                selectionMode = ListViewSelectionMode::Single,
                itemContainerStyle = themeRowLook,
                itemsSource = BindOutput {themes_.choices(), [&actions = actions_](ThemeChoice const& choice) {
                    return Grid {
                        automationName = choice.name,
                        columnDefinitions = u"*,auto,auto",
                        TextBlock {
                            choice.name,
                            column = 0,
                            fontSize = 13,
                            foreground = kChromeInk,
                            vAlign.center,
                            oneLineLook,
                        },
                        Button {
                            glyphButtonLook,
                            u"",   // шестерёнка Segoe Fluent Icons
                            column = 1,
                            toolTip = core::format(u"Настроить подложку «{}»", choice.name),
                            visibility = choice.skin ? Visibility::Visible : Visibility::Collapsed,
                            Margin{6, 0, 0, 0},
                            onClick = [&actions, skinName = choice.name] { actions.editSkin(skinName); },
                        },
                        Button {
                            glyphButtonLook,
                            u"",   // корзина оттуда же
                            column = 2,
                            toolTip = core::format(u"Удалить обложку «{}»", choice.name),
                            visibility = choice.removable ? Visibility::Visible : Visibility::Collapsed,
                            Margin{6, 0, 0, 0},
                            onClick = [&actions, skinName = choice.name] { actions.deleteSkin(skinName); },
                        },
                    };
                }},
                // Выбор — после строк: список ставит выбранную среди тех, что
                // у него уже есть.
                selectedIndex = Bind{chosen_},
            },

            // Дорога в мастер — последней строкой, после всех тем.
            Button {
                drawerQuietLook,
                u"Добавить обложку…",
                fontSize = 13,
                Margin{0, 6, 0, 0},
                Padding{12, 6},
                onClick = method(&actions_, &Actions::addSkin),
            },

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

/* ---------------- списки ---------------- */

void ReaderPanel::searchKeyDown(TextBox const&, KeyRoutedEventArgs& args) {
    if (args.key() != VirtualKey::Enter) return;
    search_.run(view_.blocks());
    args.handled(true);
}

void ReaderPanel::headingClicked(ListView const&, ItemClickEventArgs& args) {
    if (ContentsEntry const* heading = boundItem(places_.contents(), args.clickedItem())) {
        view_.goToCharOffset(heading->charOffset);
    }
}

void ReaderPanel::hitClicked(ListView const&, ItemClickEventArgs& args) {
    if (SearchHit const* hit = boundItem(search_.hits(), args.clickedItem())) {
        view_.goToCharOffset(hit->charOffset);
    }
}

void ReaderPanel::bookmarkClicked(ListView const&, ItemClickEventArgs& args) {
    if (Bookmark const* mark = boundItem(places_.bookmarks(), args.clickedItem())) {
        view_.goToCharOffset(mark->charOffset);
    }
}

/* ---------------- выбор темы ---------------- */

int ReaderPanel::selectionOf(int theme) const {
    // Номер темы за краем строк бывает на миг: список тем уже укоротился
    // (удалили обложку), а полоса ещё не поправила свой номер. Выбор такой
    // строки список не принял бы.
    return theme >= 0 && theme < themes_.count() ? theme : -1;
}

void ReaderPanel::themeChanged(int index) noexcept {
    chosen_.set(selectionOf(index));
}

void ReaderPanel::choicesChanged(list_change const&) noexcept {
    // Строки уже новые, и список, собрав их, снял выбор — и, может быть,
    // записал -1 в chosen_ (themeChosen его отбросил). Выбор возвращается из
    // темы полосы. Сам список здесь не трогают: это его же уведомление.
    chosen_.set(selectionOf(view_.theme.get()));
}

void ReaderPanel::themeChosen(int index) noexcept {
    if (index < 0) return;   // список снял выбор сам — тема та же
    view_.setTheme(index);
}

}  // namespace bukvitsa::reader
