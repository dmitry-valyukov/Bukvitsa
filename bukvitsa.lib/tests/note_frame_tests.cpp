// Тесты меры всплывающей сноски (note_frame.h) на масштабах экрана, с которыми
// живёт читалка. Разметка XAML округляет размер всплывашки и толщину рамки до
// целых пикселей, и мера обязана держать два обещания при каждом из масштабов:
// справа от колонки в окне прокрутки остаётся поле, которого хватает раскрытой
// полосе прокрутки и чернилам за шагом, а тело, которое помещается, не
// прокручивается. На экране нарушение первого видно срезанным концом строки,
// второго — короткой сноской, которую можно прокрутить на пиксель. Что строки
// тела не выходят пером и шагом за колонку, на которую их верстали, — проба
// вёрстки (typography_tests, «сноска: перья и шаги внутри колонки»).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "check.h"

#include "bukvitsa/reader/note_frame.h"

using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;

namespace {

/// Масштабы экрана: обычный, дробные, при которых рамка в 1 DIP округляется
/// вниз (1,25; 2,25) и вверх (1,5; 1,75).
constexpr float kScales[] = {1.0f, 1.25f, 1.5f, 1.75f, 2.25f};

/// Ширины всплывашки: самая узкая (220), дробные, какие даёт доля полосы, и
/// самая широкая (460).
constexpr float kPopupWidths[] = {220.0f, 287.4f, 333.7f, 401.15f, 460.0f};

/// Роста тела — дробные, как у вёрстки: строки по 1,35 кегля, отбивка 0,4. У
/// 32,3 и 36,4 при масштабе 1,75 подходящий рост всплывашки — ровно граница
/// пикселя (117 и 124), а во float он возвращается из DIP на волос ниже её: без
/// запаса меры округление вниз отняло бы у окна пиксель.
constexpr float kBodyHeights[] = {6.8f, 23.37f, 32.3f, 36.4f, 100.0f, 457.77f, 1234.56f};

/// Предел роста, до которого тело не доходит.
constexpr float kNoLimit = 1.0e6f;

/// Сколько поля должно остаться в окне справа от колонки, DIP. Раскрытая
/// полоса прокрутки WinUI — 12 DIP с отступом в 1 от края окна — ложится
/// поверх окна и колонку закрывать не должна; чернила глифа за его шагом
/// (курсивная прописная в конце строки — до 2 DIP в кегле сноски) лежат в том
/// же поле, под ней.
constexpr float kRightMargin = 13.0f;

/// Три правила округления до пикселя: разметка описана округлением к
/// ближайшему, а мера роста обещает окно не ниже подложки и при двух других.
enum class Rounding { Down, Nearest, Up };

int32_t pixelsBy(Rounding rounding, float dips, float scale) {
    if (rounding == Rounding::Down) return static_cast<int32_t>(std::floor(dips * scale));
    if (rounding == Rounding::Up) return static_cast<int32_t>(std::ceil(dips * scale));
    return layoutPixels(dips, scale);
}

void testLayoutRounding() {
    std::printf("\n=== мера сноски: округление разметки ===\n");

    check(layoutPixels(kNoteBorder, 1.0f) == 1 && layoutPixels(kNoteBorder, 1.25f) == 1 &&
              layoutPixels(kNoteBorder, 1.5f) == 2 && layoutPixels(kNoteBorder, 1.75f) == 2 &&
              layoutPixels(kNoteBorder, 2.25f) == 2,
          "рамка в 1 DIP — 1, 1, 2, 2 и 2 пикселя при масштабе 1; 1,25; 1,5; 1,75; 2,25");

    // Окно — всплывашка без рамок в пикселях: при 1,5 рамки по два, и окно на
    // пиксель уже, чем полторы ширины без двух DIP.
    const NoteFrame wide = noteFrame(460.0f, 100.0f, kNoLimit, 1.5f);
    std::printf("  всплывашка 460 при 1,5: окно %d пикселей\n", static_cast<int>(wide.windowWidth));
    check(wide.windowWidth == 686, "окно всплывашки 460 при 1,5 — 690 пикселей без двух рамок по два: 686");

    // Рост «подложка и две рамки» в DIP при 1,5 даёт окно на пиксель ниже
    // подложки: две рамки в DIP — три пикселя, а у разметки — четыре.
    bool naiveScrolls = true;
    for (const float bodyHeight : kBodyHeights) {
        const float paperHeight = notePaperHeight(bodyHeight);
        const int32_t naiveWindow =
            layoutPixels(paperHeight + 2.0f * kNoteBorder, 1.5f) - 2 * layoutPixels(kNoteBorder, 1.5f);
        if (naiveWindow >= layoutPixels(paperHeight, 1.5f)) naiveScrolls = false;
    }
    check(naiveScrolls, "рост «подложка и две рамки» в DIP при 1,5 дал бы окно ниже подложки — мера считает в пикселях");
}

void testWindowKeepsMargin() {
    std::printf("\n=== мера сноски: поле справа от колонки ===\n");

    float narrowest = kNoLimit;
    bool marginsKept = true;
    for (const float scale : kScales) {
        for (const float popupWidth : kPopupWidths) {
            const NoteFrame frame = noteFrame(popupWidth, 100.0f, kNoLimit, scale);
            // Колонка начинается с поля от левого края окна: туда тело ставит
            // рисунок, а не разметка.
            const float windowWidth = static_cast<float>(frame.windowWidth) / scale;
            const float rightMargin = windowWidth - (kNotePadding + noteColumn(popupWidth));
            narrowest = std::min(narrowest, rightMargin);
            if (rightMargin < kRightMargin) marginsKept = false;
        }
    }
    std::printf("  поле справа от колонки — не уже %.2f DIP\n", narrowest);
    check(marginsKept,
          "при каждом масштабе и ширине справа от колонки в окне не меньше 13 DIP: полоса прокрутки и чернила "
          "за шагом колонку не закрывают и не срезаются");
}

void testFittingBodyDoesNotScroll() {
    std::printf("\n=== мера сноски: тело, которое помещается ===\n");

    bool layoutAgrees = true;
    bool nearestFits = true;
    bool anyRoundingFits = true;
    int32_t mostSpare = 0;
    for (const float scale : kScales) {
        const int32_t borders = 2 * layoutPixels(kNoteBorder, scale);
        for (const float popupWidth : kPopupWidths) {
            for (const float bodyHeight : kBodyHeights) {
                const NoteFrame frame = noteFrame(popupWidth, bodyHeight, kNoLimit, scale);

                // Мера говорит о себе то же, что о ней скажет разметка.
                if (frame.paperPixels != layoutPixels(frame.paperHeight, scale) ||
                    frame.windowHeight != layoutPixels(frame.popupHeight, scale) - borders ||
                    frame.paperHeight != notePaperHeight(bodyHeight))
                    layoutAgrees = false;

                if (frame.windowHeight < frame.paperPixels) nearestFits = false;
                mostSpare = std::max(mostSpare, frame.windowHeight - frame.paperPixels);

                // Рост всплывашки и подложки разметка вправе округлить и иначе.
                for (const Rounding popupRounding : {Rounding::Down, Rounding::Nearest, Rounding::Up}) {
                    for (const Rounding paperRounding : {Rounding::Nearest, Rounding::Up}) {
                        const int32_t window = pixelsBy(popupRounding, frame.popupHeight, scale) - borders;
                        if (window < pixelsBy(paperRounding, frame.paperHeight, scale))
                            anyRoundingFits = false;
                    }
                }
            }
        }
    }
    std::printf("  запас окна над подложкой — до %d пикс.\n", static_cast<int>(mostSpare));
    check(layoutAgrees, "подложка и окно в пикселях — те, что даст разметка из роста в DIP");
    check(nearestFits && mostSpare <= 1,
          "тело помещается — окно не ниже подложки и выше её не больше чем на пиксель: прокручивать нечего");
    check(anyRoundingFits, "окно не ниже подложки, округли разметка оба роста вниз, к ближайшему или вверх");
}

void testTallBodyScrolls() {
    std::printf("\n=== мера сноски: тело выше предела ===\n");

    bool limited = true;
    for (const float scale : kScales) {
        const NoteFrame frame = noteFrame(460.0f, 1234.56f, 300.0f, scale);
        if (frame.popupHeight != 300.0f || frame.windowHeight >= frame.paperPixels ||
            frame.windowHeight != layoutPixels(300.0f, scale) - 2 * layoutPixels(kNoteBorder, scale))
            limited = false;
    }
    check(limited, "тело выше предела — всплывашка ростом с предел, окно ниже подложки: тело прокручивается");
}

}  // namespace

void runNoteFrameTests() {
    testLayoutRounding();
    testWindowKeepsMargin();
    testFittingBodyDoesNotScroll();
    testTallBodyScrolls();
}
