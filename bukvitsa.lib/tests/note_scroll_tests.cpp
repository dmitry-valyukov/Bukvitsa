// Тесты прокрутки сноски с клавиатуры (note_scroll.h): шаг на окно встаёт по
// границе строки, шаг на строку — на соседнюю, Home и End — на края, короткая
// сноска не прокручивается. На экране нарушение видно строкой, срезанной у
// края окна после PgDn, или клавишей, которая стоит на месте.
//
// Тела — строки без глифов: прокрутке нужны только их края.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "check.h"

#include "bukvitsa/reader/note_frame.h"

// Последним: строки тела — из заголовков вёрстки, они ведут к импорту модулей,
// после которого стандартный заголовок MSVC уже не принимает.
#include "bukvitsa/reader/note_scroll.h"

using namespace bukvitsa;
using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;

namespace {

/// Как строка лежит в теле: подъём, вынос, шаг с интерлиньяжем и отбивка
/// над ней, DIP. Интерлиньяж делится поровну над подъёмом и под выносом.
struct LineShape {
    float ascent = 0.0f;
    float descent = 0.0f;
    float height = 0.0f;
    float gapBefore = 0.0f;
};

/// Тело сноски строками этих форм, сверху вниз, и рост подложки под ним.
struct Body {
    std::vector<typography::NoteLine> lines;
    double paper = 0.0;
};

Body stacked(std::initializer_list<LineShape> shapes) {
    Body body;
    float slotTop = 0.0f;
    for (const LineShape& shape : shapes) {
        slotTop += shape.gapBefore;
        typography::NoteLine noteLine;
        noteLine.line.ascent = shape.ascent;
        noteLine.line.descent = shape.descent;
        noteLine.line.height = shape.height;
        noteLine.baseline = slotTop + (shape.height - shape.ascent - shape.descent) * 0.5f + shape.ascent;
        body.lines.push_back(noteLine);
        slotTop += shape.height;
    }
    body.paper = static_cast<double>(slotTop) + 2.0 * kNotePadding;
    return body;
}

/// Строка текста сноски: подъём 12, вынос 4, шаг 20 — по 2 интерлиньяжа над и
/// под ней. Отбивка абзаца — 8.
constexpr auto kText = LineShape{12.0f, 4.0f, 20.0f};
constexpr auto kParagraphStart = LineShape{12.0f, 4.0f, 20.0f, 8.0f};

/// Десять ровных строк: тело 200, подложка с полем над и под ним — 232; строка
/// i на подложке — от 16 + 20·i до 36 + 20·i.
constexpr int kLineCount = 10;
constexpr double kPaper = static_cast<double>(kLineCount) * kText.height + 2.0 * kNotePadding;

/// Окно прокрутки: четыре с половиной строки — нижнюю оно срезает.
constexpr double kWindow = 90.0;

double topOfLine(int index) {
    return kNotePadding + static_cast<double>(kText.height) * index;
}

double bottomOfLine(int index) {
    return topOfLine(index + 1);
}

std::vector<typography::NoteLine> evenBody() {
    return stacked({kText, kText, kText, kText, kText, kText, kText, kText, kText, kText}).lines;
}

double target(const std::vector<typography::NoteLine>& bodyLines, double viewTop, NoteScroll scroll,
              double windowHeight = kWindow) {
    return noteScrollTarget(bodyLines, viewTop, windowHeight, kPaper, scroll);
}

/// Окно вниз: строка, срезанная снизу, — новая верхняя; окно вверх: срезанная
/// сверху — новая нижняя; туда и обратно — на то же место.
void testPageAtLineEdge() {
    std::printf("\n=== прокрутка сноски: окно по границе строки ===\n");
    const std::vector<typography::NoteLine> bodyLines = evenBody();

    // С начала окно видит до 90: строка 3 (76–92) срезана снизу.
    check(target(bodyLines, 0.0, NoteScroll::PageDown) == topOfLine(3),
          "PgDn с начала — срезанная снизу строка встаёт наверх");
    check(target(bodyLines, topOfLine(3), NoteScroll::PageDown) == topOfLine(7),
          "PgDn ещё раз — снова до срезанной строки");

    // С 156 окно видит 156–246: строка 6 (136–152) — последняя выше окна.
    const double upTarget = target(bodyLines, topOfLine(7), NoteScroll::PageUp);
    check(upTarget == bottomOfLine(6) - kWindow, "PgUp — строка над окном встаёт вниз целиком");
    check(target(bodyLines, upTarget, NoteScroll::PageDown) == topOfLine(7),
          "PgUp и PgDn возвращают на ту же строку");
    check(target(bodyLines, topOfLine(3), NoteScroll::PageUp) == 0.0, "PgUp у начала — не выше нуля");

    // Окно встаёт в целые пиксели: верх на долю пикселя от края строки — тот
    // же край.
    check(target(bodyLines, topOfLine(3) - 0.2, NoteScroll::PageDown) == topOfLine(7),
          "верх на волос выше края строки — PgDn шагает так же");
}

/// Строк за краем окна нет или строка выше окна — шаг на окно без строки, но
/// не меньше полуокна, и всегда вперёд.
void testPageWithoutEdgeLine() {
    std::printf("\n=== прокрутка сноски: окно без строки на краю ===\n");
    const std::vector<typography::NoteLine> bodyLines = evenBody();

    // С 130 окно видит до 220: последняя строка (196–212) целиком, ниже —
    // только поле.
    check(target(bodyLines, 130.0, NoteScroll::PageDown) == 130.0 + kWindow - kText.height,
          "под окном одно поле — шаг на окно без строки");

    // Окно ниже строки: строка 0 (16–32) не помещается в 10 DIP.
    constexpr double kNarrow = 10.0;
    check(target(bodyLines, topOfLine(0), NoteScroll::PageDown, kNarrow) == topOfLine(0) + kNarrow * 0.5,
          "строка выше окна — шаг на полокна, но вперёд");
    check(target(bodyLines, 10.0, NoteScroll::PageUp) == 0.0, "над окном строк нет — к началу");
}

/// Строка вниз и вверх — на соседнюю строку; поле над первой прокручивается
/// вместе с ней.
void testLineSteps() {
    std::printf("\n=== прокрутка сноски: на строку ===\n");
    const std::vector<typography::NoteLine> bodyLines = evenBody();

    check(target(bodyLines, 0.0, NoteScroll::LineDown) == topOfLine(1),
          "↓ с начала — сразу ко второй строке, поле не отдельный шаг");
    check(target(bodyLines, topOfLine(1), NoteScroll::LineDown) == topOfLine(2), "↓ — на строку");
    check(target(bodyLines, topOfLine(2), NoteScroll::LineUp) == topOfLine(1), "↑ — на строку");
    check(target(bodyLines, topOfLine(1), NoteScroll::LineUp) == 0.0, "↑ со второй — к началу");
    check(target(bodyLines, 0.0, NoteScroll::LineUp) == 0.0, "↑ с начала — на месте");
    check(target(bodyLines, topOfLine(2) - 0.2, NoteScroll::LineDown) == topOfLine(3) &&
              target(bodyLines, topOfLine(2) + 0.2, NoteScroll::LineUp) == topOfLine(1),
          "верх на волос от края строки — шаг не стоит на месте");
    check(target(bodyLines, topOfLine(kLineCount - 1), NoteScroll::LineDown) == kPaper,
          "↓ с последней строки — к концу подложки");
}

/// Home — ноль, End — рост подложки: до конца прокрутки обрезает окно.
void testEdges() {
    std::printf("\n=== прокрутка сноски: Home и End ===\n");
    const std::vector<typography::NoteLine> bodyLines = evenBody();

    check(target(bodyLines, topOfLine(5), NoteScroll::ToStart) == 0.0, "Home — к началу");
    check(target(bodyLines, topOfLine(5), NoteScroll::ToEnd) == kPaper, "End — к росту подложки");
}

/// Сноска, которая помещается, не прокручивается ни одной клавишей.
void testShortNote() {
    std::printf("\n=== прокрутка сноски: короткая ===\n");
    const std::vector<typography::NoteLine> bodyLines = evenBody();

    bool stays = true;
    for (const NoteScroll scroll : {NoteScroll::LineDown, NoteScroll::LineUp, NoteScroll::PageDown,
                                    NoteScroll::PageUp, NoteScroll::ToStart, NoteScroll::ToEnd}) {
        stays = stays && noteScrollTarget(bodyLines, 0.0, kPaper, kPaper, scroll) == 0.0 &&
                noteScrollTarget(bodyLines, 0.0, kPaper + 1.0, kPaper, scroll) == 0.0;
    }
    check(stays, "окно не ниже подложки — всякая клавиша оставляет начало");
}

/// Края строки на подложке — те же, что у прокрутки: с половиной
/// интерлиньяжа.
double slotTopOf(const typography::NoteLine& noteLine) {
    const typography::Line& line = noteLine.line;
    return kNotePadding + noteLine.baseline - line.ascent - (line.height - line.ascent - line.descent) * 0.5;
}

double slotBottomOf(const typography::NoteLine& noteLine) {
    return slotTopOf(noteLine) + noteLine.line.height;
}

/// Допуск проверок свойств, DIP: тот же, что у края окна в пикселях.
constexpr double kSlack = 0.5;

/// Проход клавишей от края до края, как его увидит читатель: цель, обрезанная
/// до конца прокрутки, как обрезает окно. Каждый шаг идёт вперёд, между
/// прежним и новым видом не остаётся ни кусочка строки, проход доходит до
/// противоположного края, и всякая строка не выше окна хоть раз видна целиком.
bool walksWithoutLoss(const Body& body, NoteScroll scroll, double windowHeight) {
    const bool down = scroll == NoteScroll::PageDown || scroll == NoteScroll::LineDown;
    const double farthest = body.paper - windowHeight;
    double viewTop = down ? 0.0 : farthest;
    std::vector<double> views{viewTop};

    // Строк — десятки, шагов на строку — не больше чем по шагу на полокна.
    constexpr int kMostSteps = 1000;
    for (int stepCount = 0; down ? viewTop < farthest : viewTop > 0.0; ++stepCount) {
        if (stepCount == kMostSteps) return false;   // не дошёл до края
        const double next = std::clamp(
            noteScrollTarget(body.lines, viewTop, windowHeight, body.paper, scroll), 0.0, farthest);
        if (down ? next <= viewTop : next >= viewTop) return false;   // стоит или идёт назад

        // Виды не сошлись — между ними только поле или отбивка.
        const double gapTop = down ? viewTop + windowHeight : next + windowHeight;
        const double gapBottom = down ? next : viewTop;
        for (const typography::NoteLine& noteLine : body.lines) {
            if (gapTop < gapBottom - kSlack && slotTopOf(noteLine) < gapBottom - kSlack &&
                slotBottomOf(noteLine) > gapTop + kSlack)
                return false;
        }
        viewTop = next;
        views.push_back(viewTop);
    }

    for (const typography::NoteLine& noteLine : body.lines) {
        if (noteLine.line.height > windowHeight) continue;
        const bool seenWhole = std::any_of(views.begin(), views.end(), [&](double view) {
            return slotTopOf(noteLine) >= view - kSlack &&
                   slotBottomOf(noteLine) <= view + windowHeight + kSlack;
        });
        if (!seenWhole) return false;
    }
    return true;
}

/// Неровные тела: высокий заголовок первой строкой, строка выше окна,
/// абзацы с отбивками и мелкими строками вставок — и ровное. На каждом
/// клавиши окна и строки проходят сноску из конца в конец, ничего не теряя.
void testUnevenBodies() {
    std::printf("\n=== прокрутка сноски: неровные тела ===\n");

    constexpr auto kHeading = LineShape{30.0f, 8.0f, 44.0f};
    constexpr auto kTallFormula = LineShape{70.0f, 50.0f, 130.0f, 6.0f};
    constexpr auto kInsert = LineShape{8.0f, 3.0f, 14.0f};

    struct Case {
        const char* name;
        Body body;
    };
    const Case cases[] = {
        {"ровное", stacked({kText, kText, kText, kText, kText, kText, kText, kText, kText, kText})},
        {"заголовок первой строкой",
         stacked({kHeading, kParagraphStart, kText, kText, kText, kParagraphStart, kText, kText,
                  kText, kText})},
        {"строка выше окна",
         stacked({kText, kText, kText, kTallFormula, kParagraphStart, kText, kText, kText})},
        {"абзацы с отбивками и вставками",
         stacked({kText, kText, kInsert, kInsert, kParagraphStart, kText, kText, kParagraphStart,
                  kInsert, kText, kText, kText, kParagraphStart, kInsert, kInsert, kInsert, kText,
                  kParagraphStart, kText, kText})},
    };

    for (const Case& bodyCase : cases) {
        for (const double windowHeight : {kWindow, 57.0}) {
            const std::string what = std::string(bodyCase.name) + ", окно " +
                                     std::to_string(static_cast<int>(windowHeight));
            check(walksWithoutLoss(bodyCase.body, NoteScroll::PageDown, windowHeight) &&
                      walksWithoutLoss(bodyCase.body, NoteScroll::PageUp, windowHeight),
                  what + ": PgDn и PgUp не теряют строк, не стоят и доходят до края");
            check(walksWithoutLoss(bodyCase.body, NoteScroll::LineDown, windowHeight) &&
                      walksWithoutLoss(bodyCase.body, NoteScroll::LineUp, windowHeight),
                  what + ": ↓ и ↑ — так же");
        }
    }

    // Строка выше окна: шаг на строку не перепрыгивает её, а идёт по полуокну.
    const Body tall = stacked({kText, kText, kText, kTallFormula, kParagraphStart, kText});
    const double formulaTop = slotTopOf(tall.lines[3]);
    check(noteScrollTarget(tall.lines, formulaTop, kWindow, tall.paper, NoteScroll::LineDown) ==
              formulaTop + kWindow * 0.5,
          "↓ с верха строки выше окна — на полокна, не через неё");

    // Верх цели — с половиной интерлиньяжа над подъёмом: надстрочные чернила
    // строки, вставшей наверх, не срезаются.
    const Body headed = stacked({kText, kText, kText, kHeading, kText, kText});
    const double headingTop = slotTopOf(headed.lines[3]);
    check(noteScrollTarget(headed.lines, 0.0, kWindow, headed.paper, NoteScroll::PageDown) == headingTop &&
              headingTop < kNotePadding + headed.lines[3].baseline - kHeading.ascent,
          "срезанный заголовок встаёт наверх с интерлиньяжем над подъёмом");
}

}  // namespace

void runNoteScrollTests() {
    testPageAtLineEdge();
    testPageWithoutEdgeLine();
    testLineSteps();
    testEdges();
    testShortNote();
    testUnevenBodies();
}
