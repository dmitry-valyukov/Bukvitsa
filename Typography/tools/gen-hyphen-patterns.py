# /// script
# requires-python = "==3.13.*"
# dependencies = []
# ///
"""Таблица переносов, которую читает bukvitsa::typography::hyphenate().

Нужен uv: из корня дерева

    uv run Typography/tools/gen-hyphen-patterns.py

Скрипт скачивает образцы переносов TeX (алгоритм Лянга) для русского и
английского из репозитория hyph-utf8 и пишет `Typography/src/hyphen_patterns.inc`
— таблицу, которую включает `hyphenation.cpp`.

Версия прибита не веткой, а отпечатком: у каждого файла записан sha256, и
скачанное, ему не отвечающее, останавливает работу. Тот же запуск через год
обязан дать тот же файл, пока кто-нибудь не поднимет отпечаток намеренно.
Скачанное кладётся в кэш, так что повторный запуск в сеть не ходит.

Устройство таблицы. Образец `а1бр` говорит: встретив «абр», поставь между «а»
и «б» уровень 1; нечётный уровень разрешает перенос, чётный запрещает, и на
каждом месте побеждает наибольший. Образцы сложены в бор по символам алфавита
— не по байтам и не по кодовым точкам: алфавит обоих языков невелик, каждой
букве хватает байта, и слово переводится в номера символов на месте, без
выделения памяти. Одинаковые поддеревья склеиваются, отчего бор становится
ациклическим графом и втрое меньше.

Номер символа есть у букв латиницы и кириллицы и у двух знаков, которыми
образцы пишут край слова и составное слово, — точки и дефиса. Кодовая точка
вне алфавита (цифра, надстрочный знак, эмодзи, буква другой письменности)
номера не получает, и слово с ней образцы не размечают вовсе. Это не
упущение, а то, отчего закон цельности буквы соблюдается сам: разрыв возможен
только внутри слова из букв алфавита, а там нет ни составных знаков, ни
суррогатных пар. Знак ударения снимает со слова до образцов вёрстка
(hyphenPointsOf), и найденное место она же ставит на границу буквы.

Номера — массив hyphenSymbol, рядом с ним второй, hyphenTraits, — признаки
тех же кодовых точек до TABLE_END: у букв алфавита — заглавная, гласная и
язык, у прочих — буква это или знак, по общей категории Юникода из
unicodedata. Версия Юникода прибита (UNICODE_VERSION) и пишется в таблицу:
unicodedata у каждого Python своя, и с другой версией скрипт работать
отказывается, — uv по requires-python выше берёт 3.13, где она та самая.
От TABLE_END и дальше признаков нет, и там таблица несёт только блоки
пробелов, пунктуации и символов (SEPARATORS) — всё прочее вёрстка считает
буквой чужой письменности. Вёрстка берёт это отсюда же, чтобы свойство буквы было
записано в одном месте.
"""

import argparse
import hashlib
import pathlib
import re
import sys
import unicodedata
import urllib.request

BASE = ("https://raw.githubusercontent.com/hyphenation/tex-hyphen/master/"
        "hyph-utf8/tex/generic/hyph-utf8/patterns/tex/{name}")

# Языки в том порядке, в каком их перечисляет HyphenLanguage в hyphenation.h,
# и отпечаток файла образцов каждого.
LANGUAGES = [
    ("Russian", "hyph-ru.tex",
     "ce988fcc66e5e29d7474df5ab5b489d3f52a50ebccf570997f992d84221b922a"),
    ("English", "hyph-en-us.tex",
     "f4ffcd96c5cbc886bdad23f95dcae8edc3cd3620eae62f7946eceda97c4e68f8"),
]

ROOT = pathlib.Path(__file__).resolve().parents[2]
CACHE = pathlib.Path.home() / ".cache" / "bukvitsa-hyphen"

# Таблица символов покрывает кодовые точки до этой границы: латиницу с
# дополнением и расширениями, комбинируемые знаки, греческий и кириллицу с
# дополнением — письменности, чьи буквы встают в слово рядом с буквами
# алфавита образцов. Буквы алфавита все здесь; про прочие таблица знает, что
# это буква или знак, и только: вёрстке этого хватает, чтобы не размечать
# слово, в котором есть буква, образцам незнакомая. Дальше границы — ни букв
# алфавита, ни признаков.
TABLE_END = 0x0530

# Версия Юникода, по которой посчитаны признаки до TABLE_END. Это версия
# unicodedata того Python, которым скрипт запускают, а не скачанных файлов, и
# другая версия дала бы другую таблицу молча — поэтому несовпадение
# останавливает работу. Поднимать её — осознанно, вместе с requires-python.
# С версией таблицы графем wxl.core она не сверяется: та читает файлы Юникода
# сама и новее unicodedata в Python 3.13, и сверка не дала бы запустить
# скрипт вовсе.
UNICODE_VERSION = "15.1.0"

# От TABLE_END и дальше признаков нет, и всякая кодовая точка там для вёрстки —
# буква чужой письменности, в BMP или вне его: слово с ней не переносится, а
# не режется на куски, которые образцы разметили бы порознь. Кончают слово
# только блоки, где Юникод держит пробелы, пунктуацию и символы, что встают
# вплотную к латинице и кириллице: тире, кавычки, многоточие, узкие пробелы,
# валюта, буквоподобные знаки, стрелки, математика, геометрия, прочие символы
# и шрифт Брайля; дополнительная пунктуация (⸺), пунктуация CJK, неразрывный
# пробел нулевой ширины, служебные знаки (замена объекта и символа) и
# пиктограммы эмодзи — вместе с U+1FC00–U+1FFFD, которые Юникод держит
# свободными под будущие эмодзи и уже метит Extended_Pictographic, как и
# таблица графем wxl.core: эмодзи новых версий не станут буквами. Это блоки,
# а не общая категория — её на весь Юникод здесь не держат: буквы в этих
# блоках (надстрочные ⁱ ⁿ, буквоподобные ℓ ℕ Ω, знаки повтора CJK 々 〆) слово
# кончают вместе с символами вокруг, а пунктуация и цифры чужих письменностей
# вне блоков для вёрстки — буквы.
# Первая и последняя кодовая точка каждого блока.
SEPARATORS = [
    (0x2000, 0x2BFF),     # General Punctuation … Miscellaneous Symbols and Arrows
    (0x2E00, 0x2E7F),     # Supplemental Punctuation
    (0x3000, 0x303F),     # CJK Symbols and Punctuation
    (0xFEFF, 0xFEFF),     # ZERO WIDTH NO-BREAK SPACE
    (0xFFF0, 0xFFFF),     # Specials
    (0x1F000, 0x1FFFD),   # Mahjong Tiles … Symbols for Legacy Computing и запас эмодзи
]

# Признаки кодовой точки (hyphenTraits): заглавная и гласная — только у букв
# алфавита образцов, буква и знак — по общей категории Юникода. Номер языка
# буквы алфавита (с единицы, в порядке LANGUAGES) — в старших битах: по нему
# и выбираются образцы слова.
CAPITAL, VOWEL, LETTER, MARK = 0x01, 0x02, 0x04, 0x08
LANGUAGE_SHIFT = 4

# Гласные обоих алфавитов: часть слова без них вёрстка не отрывает. «y» в
# английском — гласная: иначе «rhythm» было бы словом без гласных вовсе.
VOWELS = set("аеёиоуыэюяaeiouy")


def source(name: str, digest: str) -> str:
    """Файл образцов: из кэша, а если его там нет — из сети, с проверкой."""
    cached = CACHE / digest[:16] / name
    if not cached.exists():
        with urllib.request.urlopen(BASE.format(name=name)) as response:
            data = response.read()
        actual = hashlib.sha256(data).hexdigest()
        if actual != digest:
            sys.exit(f"{name}: отпечаток {actual}, ждали {digest} — "
                     "образцы наверху изменились, поднимите его осознанно")
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_bytes(data)
    return cached.read_text(encoding="utf-8")


def parse(text: str) -> tuple[list[str], list[str], int, int]:
    """Образцы, исключения и минимумы букв слева и справа — из самого файла.

    Минимумы читаются из шапки до вычистки комментариев: заголовок файла ими и
    написан. Сами же блоки образцов комментарии тоже содержат, и в алфавит они
    попадать не должны — оттого `%` до конца строки вычищается везде.
    """
    left = re.search(r"^%\s+left:\s*(\d+)", text, re.M)
    right = re.search(r"^%\s+right:\s*(\d+)", text, re.M)

    text = re.sub(r"%.*", "", text)
    patterns = re.search(r"\\patterns\{(.*?)\n\}", text, re.S)
    exceptions = re.search(r"\\hyphenation\{(.*?)\n\}", text, re.S)
    return (patterns.group(1).split() if patterns else [],
            exceptions.group(1).split() if exceptions else [],
            int(left.group(1)) if left else 2,
            int(right.group(1)) if right else 2)


def split_pattern(pattern: str) -> tuple[str, list[tuple[int, int]]]:
    """Образец `а1бр` — это буквы «абр» и уровень 1 на первом стыке."""
    letters, levels = "", []
    for character in pattern:
        if character.isdigit():
            levels.append((len(letters), int(character)))
        else:
            letters += character
    return letters, levels


class Trie:
    """Бор образцов одного языка; узел — словарь переходов и список уровней."""

    def __init__(self) -> None:
        self.edges: dict[int, "Trie"] = {}
        self.levels: list[tuple[int, int]] = []

    def add(self, symbols: list[int], levels: list[tuple[int, int]]) -> None:
        node = self
        for symbol in symbols:
            node = node.edges.setdefault(symbol, Trie())
        node.levels = levels


def minimize(node: Trie, known: dict, order: list) -> tuple:
    """Склейка одинаковых поддеревьев: бор становится графом.

    Ключ узла — его уровни и ключи детей, поэтому равны ровно те узлы, что
    отвечают одинаково на любое продолжение слова. `order` собирает узлы от
    листьев к корню — в этом порядке их и раскладывают в байты, чтобы смещение
    ребёнка было известно раньше, чем понадобится родителю.
    """
    key = (tuple(node.levels),
           tuple((symbol, minimize(child, known, order))
                 for symbol, child in sorted(node.edges.items())))
    if key not in known:
        known[key] = len(order)
        order.append(key)
    return known[key]


def hyphenate(word: str, patterns: dict, exceptions: dict, left: int, right: int) -> list[int]:
    """Лянг как он есть, на словаре образцов, — эталон для теста движка.

    Тем и ценен, что написан прямо по описанию и ничего не знает ни о боре, ни
    о его упаковке: C++ обязан совпасть с ним слово в слово, и любое
    расхождение — это ошибка укладки таблицы или обхода, а не разночтение
    образцов.
    """
    if word in exceptions:
        points = exceptions[word]
    else:
        padded = "." + word + "."
        levels = [0] * (len(padded) + 1)
        for start in range(len(padded) - 1):
            for end in range(start + 1, len(padded) + 1):
                for position, level in patterns.get(padded[start:end], ()):
                    levels[start + position] = max(levels[start + position], level)
        points = [at - 1 for at, level in enumerate(levels) if level % 2]
    return [at for at in points if left <= at <= len(word) - right]


def words_of(root: pathlib.Path) -> list[str]:
    """Слова из книг в FB3/testdata: корпус берётся из настоящего текста, а не
    придумывается — переносам важно то, что в книгах и встречается."""
    import zipfile

    seen: dict[str, None] = {}
    for book in sorted((root / "FB3" / "testdata").glob("*.fb3")):
        with zipfile.ZipFile(book) as package:
            for entry in package.namelist():
                if not entry.endswith("body.xml"):
                    continue
                text = package.read(entry).decode("utf-8", "replace")
                text = re.sub(r"<[^>]*>", " ", text)
                for word in re.findall(r"[А-Яа-яЁёA-Za-z]{4,}", text):
                    seen.setdefault(word.lower(), None)
    return list(seen)


def rows(values: list[str], per_row: int) -> str:
    """Столбцы по ширине строки: `.inc` читают глазами не реже, чем компилятором."""
    return "\n".join("    " + ", ".join(values[at:at + per_row]) + ","
                     for at in range(0, len(values), per_row))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=pathlib.Path,
                        default=ROOT / "Typography" / "src" / "hyphen_patterns.inc")
    parser.add_argument("--corpus", type=pathlib.Path,
                        default=ROOT / "Typography" / "tests" / "data" / "hyphen_corpus.txt",
                        help="эталон для теста: слова книг из FB3/testdata с их переносами")
    parser.add_argument("--corpus-step", type=int, default=3,
                        help="брать каждое N-е слово корпуса")
    arguments = parser.parse_args()

    if unicodedata.unidata_version != UNICODE_VERSION:
        sys.exit(f"unicodedata этого Python — Юникод {unicodedata.unidata_version}, а таблица "
                 f"считается по {UNICODE_VERSION}: запустите скрипт через uv (он возьмёт "
                 "Python из requires-python) или поднимите UNICODE_VERSION осознанно")

    previous = TABLE_END - 1
    for first, last in SEPARATORS:
        if not previous < first <= last <= 0x10FFFF or (first <= 0xDFFF and last >= 0xD800):
            sys.exit(f"блок U+{first:04X}–U+{last:04X}: блоки идут по порядку, выше "
                     "таблицы, не задевая друг друга и суррогатов")
        previous = last

    languages = []
    for name, file, digest in LANGUAGES:
        patterns, exceptions, left, right = parse(source(file, digest))
        languages.append((name, file, patterns, exceptions, left, right))

    # Алфавит — объединение по языкам: номера кодовых точек общие на все
    # языки, и номер символа не зависит от того, каким языком слово размечают.
    letters = sorted({character
                      for _, _, patterns, _, _, _ in languages
                      for pattern in patterns
                      for character in pattern if not character.isdigit()}
                     | {character
                        for _, _, _, exceptions, _, _ in languages
                        for word in exceptions
                        for character in word if character != "-"})
    symbol = {character: index + 1 for index, character in enumerate(letters)}

    # Бор каждого языка, склеенный общим списком узлов: русские и английские
    # поддеревья, совпадающие целиком, хранятся один раз.
    known: dict = {}
    order: list = []
    roots = []
    for _, _, patterns, _, _, _ in languages:
        trie = Trie()
        for pattern in patterns:
            word, levels = split_pattern(pattern)
            trie.add([symbol[character] for character in word], levels)
        roots.append(minimize(trie, known, order))

    # Раскладка в байты. Узлы идут от листьев к корню, поэтому смещение
    # ребёнка уже известно, когда доходит очередь до родителя.
    offsets: list[int] = []
    # Пустой узел в самом начале: так ни один настоящий узел не окажется по
    # нулевому смещению, и нуль свободен означать «перехода нет».
    data = bytearray(b"\x00\x00")
    for levels, edges in order:
        offsets.append(len(data))
        data += bytes([len(edges), len(levels)])
        for symbol_id, child in edges:
            child_offset = offsets[child]
            data += bytes([symbol_id, child_offset & 0xFF,
                           (child_offset >> 8) & 0xFF, (child_offset >> 16) & 0xFF])
        for position, level in levels:
            data += bytes([position, level])
    if len(data) > 0xFFFFFF:
        sys.exit("бор не помещается в трёхбайтовые смещения")

    # Исключения: слова целиком, с готовой маской разрешённых мест. Хранятся
    # номерами символов и по возрастанию, чтобы искать их двоичным поиском тем
    # же сравнением, что и разметку слова.
    exception_data = bytearray()
    exception_index: list[int] = []
    exception_bounds = [0]
    for _, _, _, exceptions, _, _ in languages:
        entries = []
        for word in exceptions:
            mask, letters_only = 0, ""
            for character in word:
                if character == "-":
                    mask |= 1 << len(letters_only)
                else:
                    letters_only += character
            entries.append(([symbol[character] for character in letters_only], mask))
        for symbols, mask in sorted(entries):
            exception_index.append(len(exception_data))
            exception_data += bytes([len(symbols)]) + bytes(symbols)
            exception_data += mask.to_bytes(4, "little")
        exception_bounds.append(len(exception_index))

    header = f"""// Собрано Typography/tools/gen-hyphen-patterns.py из образцов переносов
// hyph-utf8: {', '.join(file for _, file, _, _, _, _ in languages)}.
// Править руками нечего: поменялись образцы — поднимите отпечаток в скрипте и
// запустите его через uv.
//
// Образцы русского: Copyright (C) 1999-2003 Alexander I. Lebedev, LPPL 1.2
// или новее. Английские — Copyright (C) 1990 Gerard D. C. Kuiken и
// Donald E. Knuth, свободны к использованию. http://www.hyphenation.org/tex
//
// Бор по номерам символов, склеенный до графа: узел — число переходов, число
// уровней, затем переходы (символ и трёхбайтовое смещение ребёнка) и уровни
// (место в образце и величина).
"""

    out = [header]
    out.append(f"\ninline constexpr std::uint8_t hyphenBoundarySymbol = {symbol['.']};")
    out.append(f"inline constexpr std::uint8_t hyphenSymbolCount = {len(symbol)};\n")

    # Буквы каждого языка — те, что встречаются в его образцах и исключениях.
    language_letters = [{character
                         for pattern in patterns + exceptions
                         for character in pattern if character.isalpha()}
                        for _, _, patterns, exceptions, _, _ in languages]

    # Номер символа и признаки каждой кодовой точки до TABLE_END. Регистр
    # складывается здесь же: прописной букве стоит номер строчной, «Ё» —
    # номер «ё», а не «е».
    symbols_table, traits_table = [], []
    for code_point in range(TABLE_END):
        character = chr(code_point)
        lower = character.lower()
        number = symbol.get(lower, 0) if len(lower) == 1 else 0
        category = unicodedata.category(character)

        traits = LETTER if category.startswith("L") else MARK if category.startswith("M") else 0
        owners = [index for index, letters in enumerate(language_letters) if lower in letters]
        if number and owners:
            if len(owners) != 1:
                sys.exit(f"U+{code_point:04X}: буква двух языков сразу")
            traits |= (owners[0] + 1) << LANGUAGE_SHIFT
            if character != lower:
                traits |= CAPITAL
            if lower in VOWELS:
                traits |= VOWEL
        symbols_table.append(str(number))
        traits_table.append(f"0x{traits:02X}")

    # Номер символа, но не язык — только у точки и дефиса: они знаки образцов,
    # а не буквы. Всё прочее с номером — буква ровно одного языка.
    numbered = {chr(code_point) for code_point in range(TABLE_END)
                if symbols_table[code_point] != "0"}
    if {character for character in numbered
        if not int(traits_table[ord(character)], 16) >> LANGUAGE_SHIFT} != {".", "-"}:
        sys.exit("номер символа без языка есть не только у точки и дефиса")

    out.append(f"// Свойства букв — общая категория Юникода {UNICODE_VERSION} (unicodedata).")
    out.append(f"inline constexpr std::uint32_t hyphenTableEnd = 0x{TABLE_END:04X};")
    out.append(f"inline constexpr std::uint8_t hyphenCapital = 0x{CAPITAL:02X};")
    out.append(f"inline constexpr std::uint8_t hyphenVowel = 0x{VOWEL:02X};")
    out.append(f"inline constexpr std::uint8_t hyphenLetter = 0x{LETTER:02X};")
    out.append(f"inline constexpr std::uint8_t hyphenMark = 0x{MARK:02X};")
    out.append(f"inline constexpr std::uint8_t hyphenLanguageShift = {LANGUAGE_SHIFT};\n")
    out.append(f"inline constexpr std::uint8_t hyphenSymbol[{TABLE_END}] = {{")
    out.append(rows(symbols_table, 24))
    out.append("};\n")
    out.append(f"inline constexpr std::uint8_t hyphenTraits[{TABLE_END}] = {{")
    out.append(rows(traits_table, 16))
    out.append("};\n")

    # Буквы, которые блоки пробелов, пунктуации и символов делают границей
    # слова, — сколько их, видно при каждом запуске.
    boundary_letters = sum(unicodedata.category(chr(code_point)).startswith("L")
                           for first, last in SEPARATORS
                           for code_point in range(first, last + 1))
    out.append("// От hyphenTableEnd и дальше слово кончают только эти блоки: пробелы,")
    out.append("// пунктуация и символы; прочее там — буква чужой письменности. Первая и")
    out.append("// последняя кодовая точка блока.")
    out.append(f"inline constexpr std::uint32_t hyphenSeparators[{len(SEPARATORS)}][2] = {{")
    out.append("\n".join(f"    {{0x{first:04X}, 0x{last:04X}}}," for first, last in SEPARATORS))
    out.append("};\n")

    out.append(f"inline constexpr std::uint8_t hyphenTrie[{len(data)}] = {{")
    out.append(rows([f"0x{byte:02X}" for byte in data], 16))
    out.append("};\n")

    # Переходы корня — таблицей по номеру символа: обход слова начинается с
    # каждого его места, и корень читается чаще всех узлов вместе взятых.
    out.append(f"inline constexpr std::uint32_t hyphenRootEdges[{len(languages)}]"
               f"[{len(symbol) + 1}] = {{")
    for index in range(len(languages)):
        root = order[roots[index]]
        edges = dict(root[1])
        out.append("    {")
        out.append(rows([str(offsets[edges[id]]) if id in edges else "0"
                         for id in range(len(symbol) + 1)], 16))
        out.append("    },")
    out.append("};\n")

    out.append("inline constexpr struct { std::uint32_t root; std::uint8_t left, right; }")
    out.append(f"    hyphenLanguages[{len(languages)}] = {{")
    for index, (name, _, _, _, left, right) in enumerate(languages):
        out.append(f"    {{{offsets[roots[index]]}, {left}, {right}}},  // {name}")
    out.append("};\n")

    out.append(f"inline constexpr std::uint8_t hyphenExceptions[{len(exception_data)}] = {{")
    out.append(rows([f"0x{byte:02X}" for byte in exception_data], 16))
    out.append("};\n")
    out.append(f"inline constexpr std::uint32_t hyphenExceptionAt[{len(exception_index)}] = {{")
    out.append(rows([str(value) for value in exception_index], 16))
    out.append("};\n")
    out.append(f"inline constexpr std::uint32_t hyphenExceptionBounds[{len(exception_bounds)}] = {{")
    out.append("    " + ", ".join(str(value) for value in exception_bounds))
    out.append("};")

    arguments.out.write_text("\n".join(out) + "\n", encoding="utf-8")

    # Эталон: слово и его переносы, посчитанные независимой реализацией выше.
    by_language = {}
    for index, (name, _, patterns, exceptions, left, right) in enumerate(languages):
        table = {}
        for pattern in patterns:
            word, levels = split_pattern(pattern)
            table[word] = levels
        table_exceptions = {}
        for word in exceptions:
            letters, points = "", []
            for character in word:
                if character == "-":
                    points.append(len(letters))
                else:
                    letters += character
            table_exceptions[letters] = points
        by_language[index] = (table, table_exceptions, left, right)

    lines = []
    for number, word in enumerate(words_of(ROOT)):
        if number % arguments.corpus_step:
            continue
        index = 0 if "а" <= word[0] <= "я" or word[0] == "ё" else 1
        table, table_exceptions, left, right = by_language[index]
        points = hyphenate(word, table, table_exceptions, left, right)
        marked = ""
        for at, character in enumerate(word):
            if at in points:
                marked += "-"
            marked += character
        lines.append(marked)

    arguments.corpus.parent.mkdir(parents=True, exist_ok=True)
    arguments.corpus.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{arguments.corpus}: {len(lines)} слов")

    print(f"{arguments.out}: {len(data)} байт бора ({len(order)} узлов), "
          f"{len(exception_data)} байт исключений")
    for index, (name, _, patterns, exceptions, left, right) in enumerate(languages):
        print(f"  {name}: {len(patterns)} образцов, {len(exceptions)} исключений, "
              f"минимум {left}/{right}")
    print(f"  от U+{TABLE_END:04X}: {len(SEPARATORS)} блоков пробелов, пунктуации и символов, "
          f"букв в них {boundary_letters} — граница слова")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
