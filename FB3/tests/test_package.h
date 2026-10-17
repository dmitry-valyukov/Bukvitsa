#pragma once

// Пакет FB3 для проб — прямо в тексте пробы, а не двоичным файлом рядом: ZIP
// без сжатия и минимум частей, которого требует формат. Общий для проб FB3 и
// вёрстки (Typography/tests берёт его отсюда): формат пакета один, и
// описывается он в одном месте.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bukvitsa::fb3::tests {

/// Часть архива: имя в нём и байты.
using PartBytes = std::pair<std::string_view, std::string_view>;

/// ZIP без сжатия: пакету OPC этого хватает.
inline std::string storedZip(std::span<const PartBytes> parts) {
    const auto crc32 = [](std::string_view data) {
        uint32_t crc = 0xFFFFFFFFu;
        for (const char byte : data) {
            crc ^= static_cast<unsigned char>(byte);
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    };
    const auto put16 = [](std::string& out, uint32_t value) {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
    };
    const auto put32 = [&](std::string& out, uint32_t value) {
        put16(out, value & 0xFFFF);
        put16(out, value >> 16);
    };
    // Сигнатура, версия, флаги, метод 0, время, дата 1980-01-01, CRC, размеры.
    const auto header = [&](std::string& out, uint32_t signature, bool central,
                            std::string_view name, std::string_view data) {
        put32(out, signature);
        if (central) put16(out, 20);
        put16(out, 20);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0);
        put16(out, 0x21);
        put32(out, crc32(data));
        put32(out, static_cast<uint32_t>(data.size()));
        put32(out, static_cast<uint32_t>(data.size()));
        put16(out, static_cast<uint32_t>(name.size()));
        put16(out, 0);
    };

    std::string zip;
    std::string directory;
    for (const auto& [name, data] : parts) {
        const auto offset = static_cast<uint32_t>(zip.size());
        header(zip, 0x04034B50u, false, name, data);
        zip += name;
        zip += data;

        header(directory, 0x02014B50u, true, name, data);
        put16(directory, 0);      // комментарий
        put16(directory, 0);      // диск
        put16(directory, 0);      // внутренние атрибуты
        put32(directory, 0);      // внешние атрибуты
        put32(directory, offset);
        directory += name;
    }

    const auto directoryOffset = static_cast<uint32_t>(zip.size());
    zip += directory;
    put32(zip, 0x06054B50u);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, static_cast<uint32_t>(parts.size()));
    put16(zip, static_cast<uint32_t>(parts.size()));
    put32(zip, static_cast<uint32_t>(directory.size()));
    put32(zip, directoryOffset);
    put16(zip, 0);
    return zip;
}

/// Файл связей из строк Relationship.
inline std::string relationships(std::string_view items) {
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)";
    xml += items;
    xml += "</Relationships>";
    return xml;
}

/// Связи пакета книги FB3: связь Book к описанию fb3/description.xml.
inline const std::string kBookRelationship = relationships(
    R"(<Relationship Id="rId1" Type="http://www.fictionbook.org/FictionBook3/relationships/Book" Target="fb3/description.xml"/>)");

/// Книга FB3 из частей пробы: тело — всегда, связи пакета — по умолчанию связь
/// Book, связи тела и прочие части (картинки) — если есть. Описание и его связь
/// с телом — минимум формата. Пустые связи тела — файла связей у тела нет вовсе.
struct BookPackage {
    std::string_view body{};
    std::string_view packageRelationships = kBookRelationship;
    std::string_view bodyRelationships{};
    std::span<const PartBytes> otherParts{};
};

inline std::string fb3Package(const BookPackage& book) {
    std::vector<PartBytes> parts = {
        {"[Content_Types].xml",
         R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Default Extension="png" ContentType="image/png"/><Override PartName="/fb3/description.xml" ContentType="application/fb3-description+xml"/><Override PartName="/fb3/body.xml" ContentType="application/fb3-body+xml"/></Types>)"},
        {"_rels/.rels", book.packageRelationships},
        {"fb3/description.xml",
         R"(<?xml version="1.0" encoding="UTF-8"?><fb3-description xmlns="http://www.fictionbook.org/FictionBook3/description" id="00000000-0000-0000-0000-000000000001" version="1.0"><title><main>Проба</main></title></fb3-description>)"},
        {"fb3/_rels/description.xml.rels",
         R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://www.fictionbook.org/FictionBook3/relationships/body" Target="body.xml"/></Relationships>)"},
        {"fb3/body.xml", book.body},
    };
    if (!book.bodyRelationships.empty()) parts.emplace_back("fb3/_rels/body.xml.rels", book.bodyRelationships);
    parts.insert(parts.end(), book.otherParts.begin(), book.otherParts.end());
    return storedZip(parts);
}

}  // namespace bukvitsa::fb3::tests
