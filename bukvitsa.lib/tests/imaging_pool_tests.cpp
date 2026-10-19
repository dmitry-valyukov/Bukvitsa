// Раскодирование снимка в системном пуле: петля на очереди диспетчера
// (sta_loop::start_dispatched), как у читалки с окном. У проб библиотеки
// петля спящая, и тело раскодирования там идёт на её рабочем потоке; здесь —
// в пуле системы, и ответ приходит очередью. Петля стартует раз на процесс,
// поэтому это своя программа; ctest сверяет её последнюю строку.

#include <objbase.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <semaphore>
#include <string>

#include "check.h"

#include "bukvitsa/reader/imaging.h"
#include "imaging_probe.h"

import wxl.async;
import wxl.core;

using namespace bukvitsa::reader;
using bukvitsa::reader::tests::check;
using bukvitsa::reader::tests::failures;
using wxl::async::cancellation_source;
using wxl::async::cancellation_token;
using wxl::async::operation_canceled_exception;
using wxl::async::sta_loop;
using wxl::async::task;

namespace {

const std::filesystem::path testdata = BUKVITSA_TESTDATA_DIR;

std::string onDisk(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Ждёт задачу так, как ждёт поток окна, — качая сообщения под очередью.
template <class T>
T run(task<T> work) {
    sta_loop::run_until([&] { return work.done(); });
    return work.result();
}

template <class T>
bool canceled(task<T> work) {
    sta_loop::run_until([&] { return work.done(); });
    try {
        static_cast<void>(work.result());
    } catch (const operation_canceled_exception&) {
        return true;
    } catch (...) {
    }
    return false;
}

/// Вердикты те же, что на месте: JPEG 800×507 — картинка, книга — нет.
void decodesInPool() {
    const std::string photo = onDisk(testdata / L"bg_paper2.jpg");
    const std::string book = onDisk(testdata / L"Turgenev_I._Spisokshkolnoy._Otcyi_I_Deti.fb3");

    const DecodedPicture decoded = run(decodeImageAsync(photo, kMaxPictureSide, cancellation_token{}));
    std::printf("pool: photo %ux%u\n", decoded.pixels.width, decoded.pixels.height);
    check(decoded.verdict.refusal == PictureRefusal::None && !decoded.pixels.empty() &&
              decoded.pixels.width == 800 && decoded.pixels.height == 507,
          "пул: jpeg — картинка 800×507");

    const DecodedPicture refused = run(decodeImageAsync(book, kMaxPictureSide, cancellation_token{}));
    check(refused.verdict.refusal == PictureRefusal::NotAnImage && refused.pixels.empty(), "пул: книга — не картинка");
}

/// Тело идёт не на потоке очереди, и отмена между полосами доходит до него:
/// у снимка две полосы (507 строк); после первой тело встаёт на ступени проб
/// и будит пробу, проба отменяет токен — ожидание кончается отменой за один
/// проход петли, хотя тело ещё стоит, — и отпускает тело: осиротевшее, оно кончается на
/// второй полосе и возвращается петле очередью.
void stopsBetweenBandsInPool() {
    const std::string photo = onDisk(testdata / L"bg_paper2.jpg");
    const DWORD queueThread = ::GetCurrentThreadId();
    const size_t before = sta_loop::outstanding();

    std::binary_semaphore firstBandCopied{0};
    std::binary_semaphore mayGoOn{0};
    DWORD bodyThread = 0;
    uint32_t bandsSeen = 0;
    cancellation_source source;
    task<DecodedPicture> decoding = bukvitsa::reader::detail::decodeImageAsync(
        photo, kMaxPictureSide, source.token(), [&](uint32_t band) {
            ++bandsSeen;
            if (band != 0) return;
            bodyThread = ::GetCurrentThreadId();
            firstBandCopied.release();
            mayGoOn.acquire();
        });

    // Отмена кладёт ожидающего в ход петли: один проход, который ничего не
    // ждёт, кончает ожидание, тело ещё стоит.
    firstBandCopied.acquire();
    source.cancel();
    sta_loop::run_pending();
    const bool endedAtOnce = decoding.done();
    check(endedAtOnce && canceled(std::move(decoding)),
          "пул: отменено после первой полосы — ожидание кончилось отменой за один проход петли");
    check(sta_loop::outstanding() == before + 1, "пул: сирота на счету петли, пока тело стоит");

    // Ступень ссылается на то, что здесь, — тело возвращается раньше, чем
    // проба уйдёт.
    mayGoOn.release();
    sta_loop::run_until([before] { return sta_loop::outstanding() == before; });
    check(bandsSeen == 1, "пул: осиротевшее тело кончилось на второй полосе");
    check(bodyThread != 0 && bodyThread != queueThread, "пул: тело шло не на потоке очереди");
}

}  // namespace

int main() {
    // Без буфера: строки проб доходят до ctest и тогда, когда процесс
    // остановится посреди.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        wxl::core::abort("imaging_pool_tests: COM не поднялся на потоке проб");
    sta_loop::start_dispatched("bukvitsa imaging pool tests: I/O");

    decodesInPool();
    stopsBetweenBandsInPool();

    sta_loop::stop();
    ::CoUninitialize();

    std::printf("\nimaging pool: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures;
}
