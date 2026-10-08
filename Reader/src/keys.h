#pragma once
// Клавиша WinUI — в нажатие карты клавиш (`bukvitsa/reader/key_map.h`).
//
// Карта живёт в библиотеке и окон не знает. Перевести VirtualKey, спросить у
// Windows, зажат ли Ctrl, и узнать, у какого элемента клавиша началась, может
// только Reader — это и есть его часть.

#include <wxl/Windows.System.Enums.h>

#include "Object.h"
#include "pch.h"

#include "bukvitsa/reader/key_map.h"

namespace bukvitsa::reader {

/// Нажатая клавиша и зажат ли Ctrl в эту минуту.
KeyPress keyPressOf(wxl::VirtualKey key);

/// Род элемента, у которого началась клавиша (`originalSource` события).
FocusOrigin focusOriginOf(wxl::Object const& source);

}  // namespace bukvitsa::reader
