// Тесты полос длинной сноски (NoteBands): что рисовать при показе и при
// прокрутке, что держать и когда отдать. На экране ошибку здесь видно только
// пустым куском сноски посреди прокрутки или остановкой на длинном
// комментарии; здесь каждое правило — строка проверки в пикселях.

#include <cstdio>
#include <vector>

#include "check.h"

#include "bukvitsa/reader/note_bands.h"

using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;

namespace {

/// Ширина поверхности сноски при масштабе 1,5 — окно прокрутки всплывашки в
/// 460 DIP: 690 пикселей без двух рамок по два (note_frame.h) — 686.
constexpr int32_t kWidth = 686;

/// Лучший предел устройства — 16381 на сторону: текстура 16384 от уровня 11_0
/// без трёх пикселей, которые композиция берёт себе.
constexpr int32_t kDeviceLimit = 16381;

NoteBandsGeometry geometry(int32_t height, int32_t viewport, int32_t band,
                           int32_t limit = kDeviceLimit, int32_t width = kWidth) {
    return {.surfaceWidth = width,
            .surfaceHeight = height,
            .viewportHeight = viewport,
            .bandHeight = band,
            .maxUpdateSide = limit};
}

/// Полоса во всю ширину, как её рисует поверхность не шире предела.
NotePixelRect band(int32_t top, int32_t height, int32_t width = kWidth) {
    return {0, top, width, height};
}

bool same(const std::vector<NotePixelRect>& got, const std::vector<NotePixelRect>& expected) {
    return got == expected;
}

/// Ни один кусок не длиннее предела ни по одной стороне.
bool withinLimit(const std::vector<NotePixelRect>& pieces, int32_t limit) {
    for (const NotePixelRect& piece : pieces) {
        if (piece.width > limit || piece.height > limit || piece.width <= 0 || piece.height <= 0)
            return false;
    }
    return true;
}

/// Короткая сноска — одна полоса ростом с саму сноску: рисуется при показе
/// целиком, прокрутка внутри неё ничего не дорисовывает и ничего не отдаёт.
void testShortNote() {
    std::printf("\n=== полосы сноски: короткая ===\n");

    NoteBands bands;
    bands.reset(geometry(600, 450, 1024));
    check(bands.bandCount() == 1, "сноска ниже полосы — одна полоса");

    const NoteBandsStep shown = bands.advance({0});
    check(same(shown.drawRects, {band(0, 600)}), "показ рисует её целиком, ростом с сноску, а не с полосу");
    check(!shown.trimNeeded, "отдавать нечего");
    check(same(shown.keepRects, {band(0, 600)}), "держится она же");

    const NoteBandsStep scrolled = bands.advance({150, 150});
    check(scrolled.drawRects.empty() && !scrolled.trimNeeded, "прокрутка внутри короткой — ни рисунка, ни отдачи");
}

/// Комментарий в десять тысяч слов при масштабе 1,5 — шестьдесят тысяч
/// пикселей, вчетверо выше лучшей текстуры: показ рисует только окно и окно
/// вперёд, каждый кусок — в пределе устройства.
void testTallerThanDevice() {
    std::printf("\n=== полосы сноски: выше предела устройства ===\n");

    NoteBands bands;
    bands.reset(geometry(60000, 825, 1024));
    check(bands.bandHeight() == 1024 && bands.bandCount() == 59, "60000 пикселей полосами по 1024 — 59 полос");

    const NoteBandsStep shown = bands.advance({0});
    check(same(shown.drawRects, {band(0, 1024), band(1024, 1024)}),
          "показ — окно [0, 825) и окно вперёд [825, 1650): полосы 0 и 1, не вся сноска");
    check(withinLimit(shown.drawRects, kDeviceLimit), "каждый кусок — в пределе устройства");
    check(!shown.trimNeeded, "при показе отдавать нечего");

    // Полоса выше предела режется до него: устройство 10_x рисует за раз 8189.
    NoteBands tall;
    tall.reset(geometry(60000, 825, 20000, 8189));
    check(tall.bandHeight() == 8189 && tall.bandCount() == 8, "полоса 20000 при пределе 8189 — по 8189, 8 полос");
    const NoteBandsStep tallShown = tall.advance({0});
    check(same(tallShown.drawRects, {band(0, 8189)}) && withinLimit(tallShown.drawRects, 8189),
          "показ — одна полоса в предел, не 20000");
    const NoteBandsStep tallEnd = tall.advance({60000 - 825});
    check(same(tallEnd.drawRects, {band(7 * 8189, 60000 - 7 * 8189)}),
          "последняя полоса ниже прочих — до конца сноски, не за него");

    // Поверхность шире предела (масштаб больше 4,78 на устройстве 9_1) — полоса кусками.
    NoteBands wide;
    wide.reset(geometry(3000, 800, 1024, 2045, 5000));
    const NoteBandsStep wideShown = wide.advance({0});
    check(same(wideShown.drawRects, {{0, 0, 2045, 1024}, {2045, 0, 2045, 1024}, {4090, 0, 910, 1024},
                                     {0, 1024, 2045, 1024}, {2045, 1024, 2045, 1024}, {4090, 1024, 910, 1024}}),
          "ширина 5000 при пределе 2045 — по три куска на полосу: 2045, 2045, 910");
    check(withinLimit(wideShown.drawRects, 2045), "и каждый кусок — в пределе");
    check(same(wideShown.keepRects, {band(0, 2048, 5000)}), "держится всё нарисованное одним прямоугольником во всю ширину");
}

/// Прыжок прокрутки в конец: рисуются полосы конца, начало отдаётся.
void testJumpToEnd() {
    std::printf("\n=== полосы сноски: прыжок в конец ===\n");

    NoteBands bands;
    bands.reset(geometry(60000, 825, 1024));
    bands.advance({0});

    const int32_t endTop = 60000 - 825;
    const NoteBandsStep jumped = bands.advance({endTop, endTop});
    check(same(jumped.drawRects, {band(56 * 1024, 1024), band(57 * 1024, 1024), band(58 * 1024, 60000 - 58 * 1024)}),
          "конец — окно назад [58350, 59175) и окно [59175, 60000): полосы 56, 57 и короткая 58");
    check(jumped.trimNeeded, "начало ушло дальше двух окон — отдать");
    check(same(jumped.keepRects, {band(56 * 1024, 60000 - 56 * 1024)}),
          "держится только конец, полосы 56–58 одним прямоугольником");

    const NoteBandsStep back = bands.advance({0, 0});
    check(same(back.drawRects, {band(0, 1024), band(1024, 1024)}), "назад в начало — начало рисуется заново");
    check(back.trimNeeded && same(back.keepRects, {band(0, 2048)}), "а конец отдаётся");
}

/// Окно прокрутки выше полосы: окно и запасы — по нескольку полос, и
/// дорисовывается только то, во что прокрутка вошла.
void testViewportLargerThanBand() {
    std::printf("\n=== полосы сноски: окно выше полосы ===\n");

    NoteBands bands;
    bands.reset(geometry(10000, 1000, 256));
    const NoteBandsStep shown = bands.advance({0});
    check(shown.drawRects.size() == 8 && shown.drawRects.front() == band(0, 256) &&
              shown.drawRects.back() == band(7 * 256, 256),
          "показ — [0, 2000): полосы 0–7");

    const NoteBandsStep nudged = bands.advance({300, 300});
    check(same(nudged.drawRects, {band(8 * 256, 256)}), "сдвиг на 300 — [0, 2300): только полоса 8");
    check(!nudged.trimNeeded, "отдавать нечего: держится [0, 3300)");

    const NoteBandsStep further = bands.advance({2500, 2500});
    check(further.drawRects.size() == 9 && further.drawRects.front() == band(9 * 256, 256) &&
              further.drawRects.back() == band(17 * 256, 256),
          "до 2500 — [1500, 4500): дорисованы полосы 9–17");
    check(further.trimNeeded && same(further.keepRects, {band(256, 17 * 256)}),
          "держится [500, 5500): полоса 0 отдана, 1–17 держатся");
}

/// Края полуоткрыты: полоса, кончающаяся там, где начинается окно, — не его;
/// последняя полоса — до конца сноски; пустой полосы в конце нет.
void testExactEdges() {
    std::printf("\n=== полосы сноски: края ===\n");

    NoteBands even;
    even.reset(geometry(2048, 512, 1024));
    check(even.bandCount() == 2, "сноска ровно в две полосы — две, без пустой третьей");

    NoteBands spare;
    spare.reset(geometry(1025, 512, 1024));
    check(spare.bandCount() == 2, "на пиксель выше полосы — две");
    const NoteBandsStep spareShown = spare.advance({0});
    check(same(spareShown.drawRects, {band(0, 1024)}), "окно вперёд кончается ровно на 1024 — полоса 1 не рисуется");
    const NoteBandsStep sparePixel = spare.advance({1});
    check(same(sparePixel.drawRects, {band(1024, 1)}), "на пиксель дальше — рисуется полоса 1 ростом в пиксель");

    NoteBands held;
    held.reset(geometry(8192, 512, 1024));
    held.advance({0});
    const NoteBandsStep stillHeld = held.advance({2047});
    check(!stillHeld.trimNeeded, "держится [1023, ...): полоса 0 ещё задета");
    const NoteBandsStep letGo = held.advance({2048});
    check(letGo.trimNeeded && same(letGo.keepRects, {band(1024, 2 * 1024)}),
          "держится [1024, ...): полоса 0 кончается на его начале — отдана, 1 и 2 держатся");

    // Край держания вперёд — так же, как назад: два окна под видом.
    NoteBands ahead;
    ahead.reset(geometry(8192, 512, 1024));
    ahead.advance({4096});
    const NoteBandsStep aheadHeld = ahead.advance({2561});
    check(!aheadHeld.trimNeeded, "держится [..., 4097): полоса 4 ещё задета");
    const NoteBandsStep aheadLetGo = ahead.advance({2560});
    check(aheadLetGo.trimNeeded && same(aheadLetGo.keepRects, {band(2048, 2 * 1024)}),
          "держится [..., 4096): полоса 4 начинается на его конце — отдана, 2 и 3 держатся");

    NoteBands beyond;
    beyond.reset(geometry(1000, 400, 1024));
    const NoteBandsStep past = beyond.advance({5000});
    check(past.drawRects.empty() && past.keepRects.empty() && !past.trimNeeded,
          "вид за концом сноски — ни рисунка, ни держания, без выхода за решётку");

    NoteBands unmeasured;
    unmeasured.reset(geometry(4000, 0, 1024));
    const NoteBandsStep blind = unmeasured.advance({1500});
    check(blind.drawRects.empty() && blind.keepRects.empty(),
          "окно нулевой высоты — отрезок [1500, 1500) пуст, ни одной полосы");

    NoteBands empty;
    empty.reset(geometry(0, 400, 1024));
    const NoteBandsStep nothing = empty.advance({0});
    check(empty.bandCount() == 0 && nothing.drawRects.empty(), "сноска без роста — ни полосы");
}

/// Прокрутка с инерцией: рисуется и у следующего кадра, и там, где она
/// встанет, и держится у обоих — место остановки не отдаётся до прихода.
void testFlingToRest() {
    std::printf("\n=== полосы сноски: инерция ===\n");

    NoteBands bands;
    bands.reset(geometry(60000, 825, 1024));
    bands.advance({0});

    const NoteBandsStep flung = bands.advance({100, 30000});
    check(flung.drawRects.size() == 3 && flung.drawRects.front() == band(28 * 1024, 1024) &&
              flung.drawRects.back() == band(30 * 1024, 1024),
          "до остановки на 30000 — [29175, 31650): полосы 28–30; у кадра 100 всё уже есть");
    check(!flung.trimNeeded, "начало ещё у следующего кадра — не отдаётся");
    check(same(flung.keepRects, {band(0, 2048), band(28 * 1024, 3 * 1024)}),
          "держатся оба места, каждое своим прямоугольником");
}

/// Замена устройства: пиксели ушли с прежним, решётка — заново, и вид
/// рисуется снова, а новый предел может быть меньше прежнего.
void testDeviceReplaced() {
    std::printf("\n=== полосы сноски: замена устройства ===\n");

    NoteBands bands;
    bands.reset(geometry(60000, 825, 4096));
    bands.advance({0});
    bands.advance({20000, 20000});

    bands.reset(geometry(60000, 825, 4096, 2045));
    check(bands.bandHeight() == 2045, "новое устройство рисует за раз 2045 — полоса по нему");
    const NoteBandsStep redrawn = bands.advance({20000});
    check(same(redrawn.drawRects, {band(9 * 2045, 2045), band(10 * 2045, 2045)}),
          "вид 20000 рисуется заново: [19175, 21650) — полосы 9 и 10");
    check(!redrawn.trimNeeded && withinLimit(redrawn.drawRects, 2045), "отдавать нечего, куски в новом пределе");

    // Та же мера — замена на устройство того же уровня или следующая сноска того же
    // роста в пикселях: решётка той же длины, но нарисованного у неё нет.
    NoteBands sameMeasure;
    sameMeasure.reset(geometry(60000, 825, 1024));
    sameMeasure.advance({0});
    sameMeasure.reset(geometry(60000, 825, 1024));
    const NoteBandsStep again = sameMeasure.advance({0});
    check(same(again.drawRects, {band(0, 1024), band(1024, 1024)}),
          "reset с той же мерой — вид рисуется заново, а не считается нарисованным");
}

}  // namespace

void runNoteBandsTests() {
    testShortNote();
    testTallerThanDevice();
    testJumpToEnd();
    testViewportLargerThanBand();
    testExactEdges();
    testFlingToRest();
    testDeviceReplaced();
}
