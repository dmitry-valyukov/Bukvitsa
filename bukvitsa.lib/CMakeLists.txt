# bukvitsa.lib — модель читалки без окна: книга и её индекс, реестр книг,
# настройки, обложки читателя, состояние книги, кэш картинок. Всё, что Reader
# показывает, живёт здесь и проверяется обычными тестами без экрана; Reader —
# только экраны, листание и полка поверх этого.
#
# Статическая библиотека на тех же основаниях, что bukvitsa::typography:
# заголовки в include/bukvitsa/reader, реализация в src, корневой pch через
# bukvitsa_consumes_wxl. Ни одного заголовка wxl.ui ни в интерфейсе, ни внутри —
# это и есть граница.

add_library(bukvitsa_lib STATIC
    src/book.cpp
    src/book_index.cpp
    src/imaging.cpp
    src/key_map.cpp
    src/library.cpp
    src/settings.cpp
    src/skins.cpp
    src/store.cpp
    src/workspace.cpp
    include/bukvitsa/reader/book.h
    include/bukvitsa/reader/book_index.h
    include/bukvitsa/reader/imaging.h
    include/bukvitsa/reader/key_map.h
    include/bukvitsa/reader/library.h
    include/bukvitsa/reader/settings.h
    include/bukvitsa/reader/skins.h
    include/bukvitsa/reader/store.h
    include/bukvitsa/reader/theme.h
    include/bukvitsa/reader/workspace.h
)
add_library(bukvitsa::lib ALIAS bukvitsa_lib)

target_include_directories(bukvitsa_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Вёрстка — в интерфейсе: book.h отдаёт страницы и блоки вёрстки, а с ней
# приходят FB3, DirectWrite и Direct2D. wxl::async — операции с файлами и
# корутины модели; wxl::xml и wxl::fmt — файлы читалки; windowscodecs —
# декодирование картинок (imaging.h).
target_link_libraries(bukvitsa_lib PUBLIC
    bukvitsa::typography
    wxl::async
    wxl::xml
    wxl::core
    wxl::fmt
    windowscodecs
)
bukvitsa_consumes_wxl(bukvitsa_lib)

if(BUKVITSA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
