add_executable(reader_tests reader_tests.cpp ../src/book_index.cpp)

# Чистые функции читалки над блоками книги — без окна, без wxl::ui. Исходник
# берётся из src как есть: вынимать его в библиотеку ради теста незачем.
target_include_directories(reader_tests PRIVATE ../src)
target_link_libraries(reader_tests PRIVATE bukvitsa::typography wxl::core)
bukvitsa_consumes_wxl(reader_tests)

add_test(NAME reader_tests COMMAND reader_tests)
