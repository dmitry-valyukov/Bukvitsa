add_executable(reader_tests reader_tests.cpp
    ../src/book_index.cpp
    ../src/skins.cpp
    ../src/store.cpp
    ../src/settings.cpp
    ../src/imaging.cpp
)

# Чистые функции читалки — над блоками книги и над кривыми обложек — без
# окна, без wxl::ui. Исходники берутся из src как есть: вынимать их в
# библиотеку ради теста незачем. За skins.cpp идут store.cpp (разбор XML),
# settings.cpp и imaging.cpp: в них живут каталог данных и каталог
# исполняемого, от которых обложка строит путь к снимку; windowscodecs —
# за imaging.cpp.
target_include_directories(reader_tests PRIVATE ../src)
target_link_libraries(reader_tests PRIVATE
    bukvitsa::typography
    wxl::xml
    wxl::fmt
    wxl::core
    windowscodecs
)
bukvitsa_consumes_wxl(reader_tests)

add_test(NAME reader_tests COMMAND reader_tests)
