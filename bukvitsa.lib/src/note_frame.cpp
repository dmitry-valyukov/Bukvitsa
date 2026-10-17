#include <algorithm>
#include <cmath>

#include "bukvitsa/reader/note_frame.h"

namespace bukvitsa::reader {

int32_t layoutPixels(float dips, float scale) {
    return static_cast<int32_t>(std::floor(dips * scale + 0.5f));
}

NoteFrame noteFrame(float popupWidth, float bodyHeight, float heightLimit, float scale) {
    // Рамка — с каждой стороны своими целыми пикселями: разметка округляет её
    // толщину, а не сумму двух.
    const int32_t borders = 2 * layoutPixels(kNoteBorder, scale);

    NoteFrame frame;
    frame.paperHeight = notePaperHeight(bodyHeight);
    frame.paperPixels = layoutPixels(frame.paperHeight, scale);
    frame.windowWidth = layoutPixels(popupWidth, scale) - borders;

    // Рост, в окне которого подложка видна целиком, считается в пикселях:
    // подложка, округлённая вверх, две рамки и четверть пикселя сверх.
    // Подложка — вверх: окно прокрутки сравнивает себя с желаемым размером
    // содержимого, а его разметка вправе округлить и вверх. Четверть — затем,
    // что рост ровно на границе пикселя, умноженный обратно на масштаб во
    // float, выходит на волос ниже или выше её: с четвертью сверху и к
    // ближайшему, и вверх, и вниз рост округляется не меньше чем в эти
    // пиксели. Так окно не ниже подложки, как бы разметка ни округлила рост
    // всплывашки и подложки; при округлении к ближайшему оно выше подложки
    // самое большее на пиксель.
    const int32_t paperCeiling = static_cast<int32_t>(std::ceil(frame.paperHeight * scale));
    const float fitting = (static_cast<float>(paperCeiling + borders) + 0.25f) / scale;
    frame.popupHeight = std::min(fitting, heightLimit);
    frame.windowHeight = layoutPixels(frame.popupHeight, scale) - borders;
    return frame;
}

}  // namespace bukvitsa::reader
