#pragma once
// Проверка без фреймворка, общая для всех файлов тестов библиотеки: каждый
// файл — свой слой модели, а счётчик неудач и печать строки у них одни.

#include <cstdio>
#include <string_view>

namespace bukvitsa::reader::tests {

inline int failures = 0;

inline void check(bool condition, std::string_view what) {
    std::printf("%s %.*s\n", condition ? "  ok  " : "FAILED", static_cast<int>(what.size()),
                what.data());
    if (!condition) ++failures;
}

}  // namespace bukvitsa::reader::tests
