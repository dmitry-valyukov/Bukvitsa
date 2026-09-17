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
// повторно уже не разбирается.
//
// Имена `wxl::core` — без квалификатора во всём коде читалки. Импорта здесь
// нет и быть не может: MSVC не кладёт `import` в предкомпилированный
// заголовок. Поэтому пространство имён объявлено пустым, а содержимое
// приходит с `import wxl.core;` в самом файле — директива `using` видит и
// то, что объявлено после неё. Целые типы — тоже без `std::`: `<cstdint>` и
// `<cstddef>` дают их и в глобальном пространстве имён.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <objbase.h>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <wrl/client.h>

namespace wxl::core {}
using namespace wxl::core;
