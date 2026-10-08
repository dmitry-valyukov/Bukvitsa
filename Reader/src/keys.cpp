#include "keys.h"

namespace bukvitsa::reader {

using namespace wxl;

namespace {

Key keyOf(VirtualKey key) {
    switch (key) {
        case VirtualKey::PageDown: return Key::PageDown;
        case VirtualKey::PageUp: return Key::PageUp;
        case VirtualKey::Right: return Key::Right;
        case VirtualKey::Left: return Key::Left;
        case VirtualKey::Down: return Key::Down;
        case VirtualKey::Up: return Key::Up;
        case VirtualKey::Space: return Key::Space;
        case VirtualKey::Home: return Key::Home;
        case VirtualKey::End: return Key::End;
        case VirtualKey::Add: return Key::Add;
        case VirtualKey::Subtract: return Key::Subtract;
        case VirtualKey::Number0: return Key::Number0;
        case VirtualKey::NumberPad0: return Key::NumberPad0;
        case VirtualKey::T: return Key::T;
        case VirtualKey::F: return Key::F;
        case VirtualKey::B: return Key::B;
        case VirtualKey::L: return Key::L;
        case VirtualKey::F2: return Key::F2;
        case VirtualKey::F11: return Key::F11;
        case VirtualKey::Escape: return Key::Escape;
        default: return Key::Other;
    }
}

}  // namespace

KeyPress keyPressOf(VirtualKey key) {
    // Модификаторов у KeyRoutedEventArgs не спросить: WinUI их там не отдаёт.
    // Состояние клавиши знает Win32, и вопрос к нему — один вызов.
    return KeyPress{keyOf(key), (::GetKeyState(VK_CONTROL) & 0x8000) != 0};
}

FocusOrigin focusOriginOf(Object const& source) {
    if (source.is<Slider>()) return FocusOrigin::Slider;
    if (source.is<TextBox>()) return FocusOrigin::TextBox;
    if (source.is<Button>()) return FocusOrigin::Button;
    return FocusOrigin::Root;
}

}  // namespace bukvitsa::reader
