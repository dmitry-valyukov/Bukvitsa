# Тесты модели читалки: линкуют саму библиотеку, как всякий её потребитель, —
# ни одного исходника из src здесь не компилируется заново.
add_executable(bukvitsa_lib_tests
    lib_tests.cpp
    book_places_tests.cpp
    book_presenter_tests.cpp
    book_search_tests.cpp
    chapter_tail_tests.cpp
    key_map_tests.cpp
    note_bands_tests.cpp
    note_frame_tests.cpp
    page_flow_tests.cpp
    pieces_tests.cpp
    settings_tests.cpp
    skin_editor_tests.cpp
    theme_list_tests.cpp
)

target_link_libraries(bukvitsa_lib_tests PRIVATE bukvitsa::lib)
bukvitsa_consumes_wxl(bukvitsa_lib_tests)

# Книги для проб — общие с FB3 и вёрсткой.
target_compile_definitions(bukvitsa_lib_tests PRIVATE
    BUKVITSA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../FB3/testdata")

add_test(NAME bukvitsa_lib_tests COMMAND bukvitsa_lib_tests)

# Раскодирование снимка в системном пуле — под петлёй на очереди диспетчера
# (start_dispatched), как у читалки с окном: петля стартует раз на процесс, а
# пробы выше идут на спящей, поэтому своя программа. Сверяется её итог.
add_executable(bukvitsa_imaging_pool_tests imaging_pool_tests.cpp)
target_link_libraries(bukvitsa_imaging_pool_tests PRIVATE bukvitsa::lib)
bukvitsa_consumes_wxl(bukvitsa_imaging_pool_tests)
target_compile_definitions(bukvitsa_imaging_pool_tests PRIVATE
    BUKVITSA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../FB3/testdata")

add_test(NAME bukvitsa_imaging_pool_tests COMMAND bukvitsa_imaging_pool_tests)
set_tests_properties(bukvitsa_imaging_pool_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "imaging pool: OK"
    FAIL_REGULAR_EXPRESSION "FAILED")
