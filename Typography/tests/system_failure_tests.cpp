// Точка расширения остановки (system_failure.h): у отказа stopIfFailed сперва
// зовёт проверку на потерю устройства — бросившая проверка доходит до
// вызывающего, вернувшаяся ведёт к остановке, — а ставится проверка раз на
// процесс: второй раз, как и пустая, — остановка.
//
// Раз на процесс — значит, и сценарий на процесс: программа одна, сценарий —
// её аргумент, и каждый — свой запуск ctest. Сценарии с остановкой ctest
// сверяет по её строке в stderr (PASS_REGULAR_EXPRESSION), без кода выхода.

#include <windows.h>

#include <d2d1_1.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "bukvitsa/typography/system_failure.h"

namespace {

using namespace bukvitsa::typography;

/// Код, которым проба называет потерю устройства, и исключение, которым её
/// проверка бросает, — как проверка читалки над wxl.
constexpr HRESULT kLostCode = D2DERR_RECREATE_TARGET;

struct ProbeDeviceLost {};

/// Остановка без окна отчёта и без WER: её слова в stderr — всё, что смотрит
/// ctest.
void withoutDialog() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

/// Бросившая проверка доходит до вызывающего stopIfFailed: так рисунок
/// поверхности узнаёт потерю, а не читалка останавливается.
int lostReachesCaller() {
    setDeviceLossCheck([](HRESULT result) {
        if (result == kLostCode) throw ProbeDeviceLost{};
    });
    try {
        stopIfFailed(kLostCode, "probe: lost device");
    } catch (const ProbeDeviceLost&) {
        std::puts("lost: the check threw, and the caller caught it");
        return 0;
    }
    std::puts("lost: stopIfFailed returned on a failure");
    return 1;
}

/// Проверка зовётся до остановки: её слово в stderr стоит перед строкой
/// остановки, в той же строке.
int checkBeforeStop() {
    withoutDialog();
    setDeviceLossCheck([](HRESULT result) {
        std::fprintf(stderr, "check 0x%08X; ", static_cast<unsigned>(result));
        std::fflush(stderr);
    });
    stopIfFailed(E_INVALIDARG, "probe: other failure");
    return 1;
}

/// Без проверки (пробы библиотеки) отказ останавливает, как всякий.
int stopsWithoutCheck() {
    withoutDialog();
    stopIfFailed(E_FAIL, "probe: no check");
    return 1;
}

int setTwice() {
    withoutDialog();
    setDeviceLossCheck([](HRESULT) {});
    setDeviceLossCheck([](HRESULT) {});
    return 1;
}

int setEmpty() {
    withoutDialog();
    setDeviceLossCheck(nullptr);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view scenario = argc > 1 ? argv[1] : "";
    if (scenario == "lost") return lostReachesCaller();
    if (scenario == "order") return checkBeforeStop();
    if (scenario == "unset") return stopsWithoutCheck();
    if (scenario == "twice") return setTwice();
    if (scenario == "empty") return setEmpty();
    std::fputs("system_failure_tests: lost | order | unset | twice | empty\n", stderr);
    return 2;
}
