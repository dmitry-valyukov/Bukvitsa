#pragma once
// Ступень проб раскодирования — только для тестов библиотеки: читалке её не
// видно. Реализация — в imaging.cpp, рядом с формой, которую она повторяет.

#include <cstdint>
#include <functional>
#include <string>

#include "bukvitsa/reader/imaging.h"

namespace bukvitsa::reader::detail {

/// `decodeImageAsync`, чьё тело после каждой скопированной полосы зовёт
/// `afterBand(номер полосы)` на потоке пула — так проба ставит отмену ровно
/// между полосами, без опроса. Тело может пережить ожидание: то, на что
/// ссылается `afterBand`, проба держит, пока сирота не вернётся.
wxl::async::task<DecodedPicture> decodeImageAsync(std::string bytes, uint32_t maxSide,
                                                  wxl::async::cancellation_token token,
                                                  std::function<void(uint32_t band)> afterBand);

}  // namespace bukvitsa::reader::detail
