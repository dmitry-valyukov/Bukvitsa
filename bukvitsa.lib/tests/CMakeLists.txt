# Тесты модели читалки: линкуют саму библиотеку, как всякий её потребитель, —
# ни одного исходника из src здесь не компилируется заново.
add_executable(bukvitsa_lib_tests lib_tests.cpp pieces_tests.cpp skin_editor_tests.cpp)

target_link_libraries(bukvitsa_lib_tests PRIVATE bukvitsa::lib)
bukvitsa_consumes_wxl(bukvitsa_lib_tests)

# Книги для проб — общие с FB3 и вёрсткой.
target_compile_definitions(bukvitsa_lib_tests PRIVATE
    BUKVITSA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../FB3/testdata")

add_test(NAME bukvitsa_lib_tests COMMAND bukvitsa_lib_tests)
