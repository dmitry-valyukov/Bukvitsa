#include "chrome.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

constexpr uint8_t kOpaque = 0xFF;
constexpr uint8_t kFace = 0xF2;     ///< под ящиком просвечивает текст
constexpr uint8_t kEdge = 0x66;
constexpr uint8_t kActive = 0x1A;   ///< отметка — тон текста, едва заметный
constexpr uint8_t kSubtle = 0x40;

Brush brushOf(D2D1_COLOR_F color, uint8_t alpha) {
    return SolidColorBrush{argbOf(color, alpha)};
}

}  // namespace

ARGB argbOf(D2D1_COLOR_F color, uint8_t alpha) {
    const auto channel = [](float value) {
        return static_cast<uint8_t>(value * 255.0f + 0.5f);
    };
    return ARGB{alpha, channel(color.r), channel(color.g), channel(color.b)};
}

intrusive_ptr<Chrome> Chrome::create(const Theme& theme) {
    return {new Chrome{theme}, /*add_ref=*/false};
}

Chrome::Chrome(const Theme& theme)
    : face{brushOf(theme.panel, kFace)},
      paper{brushOf(theme.background, kOpaque)},
      card{brushOf(theme.panel, kOpaque)},
      ink{brushOf(theme.text, kOpaque)},
      dim{brushOf(theme.dim, kOpaque)},
      edge{brushOf(theme.dim, kEdge)},
      active{brushOf(theme.text, kActive)},
      subtle{brushOf(theme.dim, kSubtle)} {}

void Chrome::follow(const Theme& theme) {
    face.set(brushOf(theme.panel, kFace));
    paper.set(brushOf(theme.background, kOpaque));
    card.set(brushOf(theme.panel, kOpaque));
    ink.set(brushOf(theme.text, kOpaque));
    dim.set(brushOf(theme.dim, kOpaque));
    edge.set(brushOf(theme.dim, kEdge));
    active.set(brushOf(theme.text, kActive));
    subtle.set(brushOf(theme.dim, kSubtle));
}

}  // namespace bukvitsa::reader
