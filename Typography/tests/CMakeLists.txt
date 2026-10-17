add_executable(typography_tests typography_tests.cpp)

# wxl::core — за STA-пулом: тест открывает настоящие книги, а разбор XML
# берёт из пула память. bukvitsa::mathml — сквозная проверка цепочки EPUB:
# MathML → TeX → MicroTeX.
target_link_libraries(typography_tests PRIVATE bukvitsa::typography bukvitsa::mathml wxl::core)

# Пакет FB3 для проб (test_package.h) — у проб FB3: верхний слой берёт у нижнего.
target_include_directories(typography_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../FB3/tests)
bukvitsa_consumes_wxl(typography_tests)

add_test(NAME typography_tests COMMAND typography_tests)

target_compile_definitions(typography_tests PRIVATE
    BUKVITSA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../FB3/testdata"
    BUKVITSA_TYPOGRAPHY_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    BUKVITSA_MICROTEX_RES="${CMAKE_CURRENT_SOURCE_DIR}/../../external/MicroTeX/res")

# windowscodecs — CLSID фабрики WIC для растеризации формулы в тесте.
target_link_libraries(typography_tests PRIVATE windowscodecs)

# Точка расширения остановки (system_failure.h) ставится раз на процесс:
# сценарий — аргумент программы, каждый — свой запуск; сценарии с остановкой
# сверяются по её строке в stderr, код выхода у них не смотрится.
add_executable(system_failure_tests system_failure_tests.cpp)
target_link_libraries(system_failure_tests PRIVATE bukvitsa::typography wxl::core)
bukvitsa_consumes_wxl(system_failure_tests)

add_test(NAME system_failure.lost COMMAND system_failure_tests lost)
add_test(NAME system_failure.order COMMAND system_failure_tests order)
add_test(NAME system_failure.unset COMMAND system_failure_tests unset)
add_test(NAME system_failure.twice COMMAND system_failure_tests twice)
add_test(NAME system_failure.empty COMMAND system_failure_tests empty)
set_tests_properties(system_failure.order PROPERTIES
    PASS_REGULAR_EXPRESSION "check 0x80070057; [^\n]*probe: other failure, HRESULT 0x80070057")
set_tests_properties(system_failure.unset PROPERTIES
    PASS_REGULAR_EXPRESSION "probe: no check, HRESULT 0x80004005")
set_tests_properties(system_failure.twice PROPERTIES
    PASS_REGULAR_EXPRESSION "setDeviceLossCheck: проверка ставится второй раз")
set_tests_properties(system_failure.empty PROPERTIES
    PASS_REGULAR_EXPRESSION "setDeviceLossCheck: пустая проверка")
