// Тесты списка тем: номер ↔ имя ↔ настройки и куда встать после удаления и
// сохранения обложки. Номер плывёт при каждой перемене реестра, а на диск
// уходят имена — здесь проверяется, что переводы между ними сходятся.

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

// Заголовок модели — последним: он несёт импорт wxl.core.
#include "check.h"

#include "bukvitsa/reader/theme_list.h"

import wxl.core;

using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;

namespace {

/// Обложка читателя: кривые начальные, снимок любой — список их не смотрит.
Skin skinNamed(u16_view name) {
    Skin skin = defaultSkin();
    skin.name = u16_text{name};
    skin.image = u16_text{u"photo.jpg"};
    return skin;
}

std::vector<Skin> registry(std::initializer_list<u16_view> names) {
    std::vector<Skin> skins;
    for (const u16_view name : names) skins.push_back(skinNamed(name));
    return skins;
}

/// Список с этими обложками читателя. Возвращается значением прямо из
/// конструктора: список держит поле строк, а поле не копируется и не
/// переезжает.
ThemeList listOf(std::initializer_list<u16_view> names) {
    return ThemeList{registry(names)};
}

/// Номер обложки, которая точно есть в списке.
int indexOf(const ThemeList& list, u16_view name) {
    return list.indexOfSkin(name).value_or(-1);
}

int systemCount() {
    return static_cast<int>(systemSkins().size());
}

void testNumbersAndNames() {
    std::printf("\n=== темы: номер и имя ===\n");

    const ThemeList bare{};
    check(bare.count() == kThemeCount, "до setSkins — одни встроенные темы");

    const ThemeList list = listOf({u"Альфа", u"Бета"});
    check(list.count() == kThemeCount + systemCount() + 2, "встроенные, системные и две читателя");

    bool themesRoundTrip = true;
    for (int index = 0; index < kThemeCount; ++index) {
        themesRoundTrip = themesRoundTrip && list.themeIdAt(index) == kThemes[index].id &&
                          list.indexOfTheme(kThemes[index].id) == index &&
                          &list.paperAt(index) == &kThemes[index] && list.skinAt(index) == nullptr;
    }
    check(themesRoundTrip, "встроенная: номер ↔ ключ, своя бумага, не обложка");
    check(list.indexOfTheme(u"Antique") == 0, "незнакомый ключ — первая тема");

    const Skin* firstSystem = list.skinAt(kThemeCount);
    check(firstSystem && firstSystem->system, "за встроенными — системные обложки");

    const int beta = indexOf(list, u"Бета");
    check(beta == kThemeCount + systemCount() + 1, "обложка читателя — за системными, в порядке реестра");
    check(list.skinAt(beta) && list.skinAt(beta)->name == u"Бета", "номер → обложка с тем же именем");
    check(list.themeIdAt(beta).empty(), "у обложки ключа темы нет");
    check(&list.paperAt(beta) == &kSkinTheme, "бумага обложки — общая палитра обложек");
    check(!list.indexOfSkin(u"Гамма"), "неизвестной обложки нет");
    check(list.skinAt(list.count()) == nullptr && list.skinAt(-1) == nullptr, "вне списка — пусто");
}

void testNormalizeWraps() {
    std::printf("\n=== темы: по кругу ===\n");

    const ThemeList list = listOf({u"Альфа"});
    const int all = list.count();

    check(list.normalize(all) == 0, "за последней обложкой — первая тема");
    check(list.normalize(-1) == all - 1, "перед первой темой — последняя обложка");
    check(list.normalize(all + 2) == 2 && list.normalize(1) == 1, "по модулю длины списка");
    check(&list.paperAt(all) == &kThemes[0], "бумага по номеру за краем — по кругу");
}

void testRemoval() {
    std::printf("\n=== темы: удаление обложки ===\n");

    const ThemeList before = listOf({u"Альфа", u"Бета", u"Гамма"});

    // Удалили текущую — встроенная по ключу из настроек, а не первая.
    check(before.afterRemoval(u"Бета", indexOf(before, u"Бета"), u"Night") == before.indexOfTheme(u"Night"),
          "удалили текущую — встроенная из настроек");
    check(before.afterRemoval(u"Гамма", indexOf(before, u"Гамма"), u"Sepia") ==
              before.indexOfTheme(u"Sepia"),
          "удалили текущую последнюю — тоже встроенная из настроек");

    // Удалили другую — та же по имени в укоротившемся списке.
    const ThemeList withoutAlpha = listOf({u"Бета", u"Гамма"});
    check(before.afterRemoval(u"Альфа", indexOf(before, u"Гамма"), u"Night") ==
              indexOf(withoutAlpha, u"Гамма"),
          "удалили стоящую раньше — та же, номер на один меньше");

    const ThemeList withoutGamma = listOf({u"Альфа", u"Бета"});
    check(before.afterRemoval(u"Гамма", indexOf(before, u"Альфа"), u"Night") ==
              indexOf(withoutGamma, u"Альфа"),
          "удалили стоящую позже — та же, номер прежний");

    check(before.afterRemoval(u"Альфа", 2, u"Day") == 2, "встроенная удалением не двигается");
    check(before.afterRemoval(u"Альфа", kThemeCount, u"Day") == kThemeCount, "системная — тоже");
    check(before.afterRemoval(u"Дельта", indexOf(before, u"Бета"), u"Day") == indexOf(before, u"Бета"),
          "удаляемой нет в списке — ничего не двигается");

    // Читатель назвал свою обложку как системную: удаляют его, не системную.
    const Skin* system = before.skinAt(kThemeCount);
    if (!system) return;
    const u16_text systemName = system->name;

    const ThemeList twin = listOf({u"Альфа", systemName});
    const int mine = kThemeCount + systemCount() + 1;
    check(twin.afterRemoval(systemName, mine, u"Night") == twin.indexOfTheme(u"Night"),
          "тёзка системной — текущая: встроенная из настроек");
    check(twin.afterRemoval(systemName, kThemeCount, u"Night") == kThemeCount,
          "системная тёзка удалением не задета");
}

void testSaveBecomesCurrent() {
    std::printf("\n=== темы: сохранённая становится текущей ===\n");

    const ThemeList after = listOf({u"Альфа", u"Бета", u"Новая"});
    check(after.afterSave(u"Новая", 1) == indexOf(after, u"Новая"), "новая обложка — текущая");
    check(after.afterSave(u"Альфа", indexOf(after, u"Бета")) == indexOf(after, u"Альфа"),
          "пересохранённая под прежним именем — текущая на прежнем месте");
    check(after.afterSave(u"Пропавшая", 2) == 2, "сохранённой нет в списке — остаётся прежняя");

    const Skin* system = after.skinAt(kThemeCount);
    if (!system) return;
    const u16_text systemName = system->name;

    const ThemeList twin = listOf({u"Альфа", systemName});
    check(twin.afterSave(systemName, 0) == kThemeCount + systemCount() + 1,
          "сохранённая под именем системной — своя, не системная");
}

void testStartFromSettings() {
    std::printf("\n=== темы: старт по настройкам ===\n");

    const ThemeList list = listOf({u"Альфа", u"Бета"});

    check(list.indexFromSettings(u"Night", u"") == list.indexOfTheme(u"Night"), "по ключу темы");
    check(list.indexFromSettings(u"Night", u"Бета") == indexOf(list, u"Бета"), "по имени обложки");
    check(list.indexFromSettings(u"Sepia", u"Стёртая") == list.indexOfTheme(u"Sepia"),
          "обложки нет в реестре — тема по ключу");
    check(list.indexFromSettings(u"Bogus", u"") == 0, "незнакомый ключ — первая тема");

    const Skin* system = list.skinAt(kThemeCount + 1);
    check(system && list.indexFromSettings(u"Day", system->name) == kThemeCount + 1,
          "системная обложка — по имени");
}

void testPersistKeepsThemeKey() {
    std::printf("\n=== темы: что пишется в настройки ===\n");

    const ThemeList list = listOf({u"Альфа", u"Бета"});

    const ThemeList::Persisted sepia = list.persist(list.indexOfTheme(u"Sepia"), u"Night");
    check(sepia.theme == u"Sepia" && sepia.skin.empty(), "встроенная — её ключ, обложки нет");

    const ThemeList::Persisted beta = list.persist(indexOf(list, u"Бета"), u"Night");
    check(beta.theme == u"Night" && beta.skin == u"Бета", "обложка — её имя, прежний ключ остаётся");

    // Записанное и прочитанное при старте сходятся для каждого номера.
    bool roundTrip = true;
    for (int index = 0; index < list.count(); ++index) {
        const ThemeList::Persisted saved = list.persist(index, u"Sepia");
        roundTrip = roundTrip && list.indexFromSettings(saved.theme, saved.skin) == index;
    }
    check(roundTrip, "persist → indexFromSettings возвращает тот же номер");

    // Удалили текущую: прежний ключ, сохранённый при обложке, и есть то,
    // куда она возвращается.
    const ThemeList::Persisted onSkin = list.persist(indexOf(list, u"Альфа"), u"Sepia");
    check(list.afterRemoval(u"Альфа", indexOf(list, u"Альфа"), onSkin.theme) == list.indexOfTheme(u"Sepia"),
          "после удаления текущей — тот ключ, что держали настройки");
}

/// Строки для показа — тем же счётом, что номер: встроенные, системные
/// обложки, обложки читателя; шестерёнка — у обложки, корзина — только у
/// обложки читателя. Новый реестр — один сброс, тот же — без перемен.
void testChoices() {
    std::printf("\n=== темы: строки для показа ===\n");

    ThemeList bare;
    check(bare.choices().size() == static_cast<uint32_t>(kThemeCount), "до setSkins — строки одних встроенных тем");

    // Слышанное — раньше списка: слушатель уходит вместе с ним.
    std::vector<list_change> heard;

    ThemeList list{registry({u"Альфа"})};
    static_cast<void>(list.choices().on_change(
        [&heard](const list_change& change) noexcept { heard.push_back(change); }));
    observable_list<ThemeChoice const>& choices = list.choices();

    check(choices.size() == static_cast<uint32_t>(list.count()), "строк столько же, сколько тем");

    bool themesFirst = true;
    for (int index = 0; index < kThemeCount; ++index) {
        const ThemeChoice& choice = choices[static_cast<uint32_t>(index)];
        themesFirst = themesFirst && choice.name == kThemes[index].name && !choice.skin && !choice.removable;
    }
    check(themesFirst, "встроенные — первыми, со своими подписями, без шестерёнки и корзины");

    bool sameCount = true;
    for (int index = kThemeCount; index < list.count(); ++index) {
        const ThemeChoice& choice = choices[static_cast<uint32_t>(index)];
        const Skin* skin = list.skinAt(index);
        sameCount = sameCount && skin && choice.name == skin->name && choice.skin && choice.removable == !skin->system;
    }
    check(sameCount, "строка n — тема n: имя обложки, шестерёнка, корзина только у своей");

    const int alpha = indexOf(list, u"Альфа");
    check(choices[static_cast<uint32_t>(alpha)].removable, "обложка читателя удаляется");
    check(!choices[static_cast<uint32_t>(kThemeCount)].removable, "системная — нет");

    list.setSkins(registry({u"Альфа", u"Бета"}));
    check(heard.size() == 1 && heard[0] == list_change{list_change::reset, 0, static_cast<uint32_t>(list.count())},
          "новый реестр — один сброс на все строки");
    check(choices[static_cast<uint32_t>(indexOf(list, u"Бета"))].name == u"Бета", "новая обложка — на своей строке");

    list.setSkins(registry({u"Альфа", u"Бета"}));
    check(heard.size() == 1, "тот же реестр ещё раз — без перемен");
}

}  // namespace

void runThemeListTests() {
    testNumbersAndNames();
    testNormalizeWraps();
    testRemoval();
    testSaveBecomesCurrent();
    testStartFromSettings();
    testPersistKeepsThemeKey();
    testChoices();
}
