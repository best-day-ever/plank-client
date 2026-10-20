#pragma once

#include <QString>
#include <plank_clipboard_file_wire.h>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace plank::clipboard::windows {

// PCF2 names are UTF-8 single components. Windows adds DOS device aliases,
// alternate streams, trailing-dot normalization and case-insensitive names.
inline bool validName(const std::string& value)
{
    if (!plank_clipboard_file_name_valid(
                reinterpret_cast<const std::uint8_t*>(value.data()), value.size())) return false;
    const auto name = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    if (name.endsWith(u'.') || name.endsWith(u' ') || name.size() > 255) return false;
    for (const auto character : name) {
        if (character.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(character))
            return false;
    }
    const auto stem = name.section(u'.', 0, 0).trimmed().toUpper();
    if (stem == QStringLiteral("CON") || stem == QStringLiteral("PRN") ||
            stem == QStringLiteral("AUX") || stem == QStringLiteral("NUL") ||
            stem == QStringLiteral("CONIN$") || stem == QStringLiteral("CONOUT$")) return false;
    if (stem.size() == 4 && (stem.startsWith(QStringLiteral("COM")) ||
                            stem.startsWith(QStringLiteral("LPT")))) {
        const auto digit = stem.at(3).unicode();
        if ((digit >= '1' && digit <= '9') || digit == 0x00b9 ||
                digit == 0x00b2 || digit == 0x00b3) return false;
    }
    return true;
}

inline QString foldedPath(const std::string& path)
{
    return QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())).toCaseFolded();
}

#ifdef _WIN32
// Reject all reparse points along a source or staging path, including junctions.
// Deliberately accept local drive paths only: UNC and Win32 device namespaces
// have different identity/normalization semantics and cannot meet this contract.
inline bool plainAbsolutePath(const std::filesystem::path& path)
{
    const auto value = path.native();
    if (value.size() < 3 || !((value[0] >= L'A' && value[0] <= L'Z') ||
                             (value[0] >= L'a' && value[0] <= L'z')) ||
            value[1] != L':' || (value[2] != L'\\' && value[2] != L'/')) return false;
    const auto type = GetDriveTypeW(path.root_path().c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) return false;
    std::filesystem::path current = path.root_path();
    for (const auto& component : path.relative_path()) {
        if (!validName(component.u8string())) return false;
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}
#endif

} // namespace plank::clipboard::windows
