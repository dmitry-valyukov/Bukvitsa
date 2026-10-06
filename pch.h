#pragma once
// Предкомпилированный заголовок Буквицы.
//
// CMake подставляет его первым в каждую обычную единицу трансляции вёрстки,
// читалки и тестов (`bukvitsa_consumes_wxl` в корневом CMakeLists.txt). В
// единицы модулей FB3 и MathML он не попадает: MSVC не принимает
// подставленный заголовок перед `module`.
//
// Стандартные и системные заголовки собраны здесь, до любого `import`:
// заголовок, включённый после импорта, MSVC не принимает, а включённый здесь
// повторно уже не разбирается. Список покрывает и то, что включают публичные
// заголовки wxl.ui (`core.h` и соседи): заголовок читалки, который сам
// импортирует wxl.core (`theme.h`, `skins.h`, `settings.h`), может стоять
// раньше их, и тогда их `<coroutine>` или `<ranges>` пришёл бы после импорта.
//
// Имена `wxl::core` — без квалификатора во всём коде читалки. Импорта здесь
// нет и быть не может: MSVC не кладёт `import` в предкомпилированный
// заголовок. Поэтому пространство имён объявлено пустым, а содержимое
// приходит с `import wxl.core;` в самом файле — директива `using` видит и
// то, что объявлено после неё. Целые типы — тоже без `std::`: `<cstdint>` и
// `<cstddef>` дают их и в глобальном пространстве имён.
//
// windows.h — первым и только через platform.h из wxl, как во всех проектах на
// ней: там же его ключи и снятый макрос GetCurrentTime.

#include "platform.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <objbase.h>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <wrl/client.h>

namespace wxl::core {}
using namespace wxl::core;
