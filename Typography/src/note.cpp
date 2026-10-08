// Сноска: тело -> блоки -> строки на базовых линиях.
//
// Стиль сноски — здесь, а не у того, кто её показывает, по той же причине, по
// какой таблица стилей блоков живёт в пагинаторе: как выглядит сноска, знает
// вёрстка, а не окно.

// Свой заголовок — единственный: он ведёт к импорту модуля книги, а
// стандартный заголовок после импорта MSVC уже не принимает.
#include "bukvitsa/typography/note.h"

namespace bukvitsa::typography {

NoteLayout layoutNote(Engine& engine, const fb3::Node& note, float width, float fontSize) {
    NoteLayout result;
    if (width <= 0.0f) return result;

    // Сноска набирается мельче книги и без абзацного отступа: она короткая, и
    // отступ в ней читался бы как случайная дыра.
    ParagraphStyle style;
    style.fontSize = fontSize * 0.85f;
    style.lineHeight = 1.35f;
    style.alignment = Alignment::Justify;

    float y = 0.0f;

    for (const Block& block : flatten(note)) {
        if (block.paragraph.text.empty()) continue;

        for (Line& line : engine.layout(block.paragraph, width, style)) {
            y += line.ascent;
            const float baseline = y;
            y += line.height - line.ascent;
            result.lines.push_back(NoteLine{std::move(line), baseline});
        }

        y += style.fontSize * 0.4f;   // отбивка между абзацами сноски
    }

    result.height = y;
    return result;
}

}  // namespace bukvitsa::typography
