#pragma once

#include <cwctype>
#include <string>
#include <string_view>

namespace FileAssociationNaming {
inline std::wstring NormalizeExtension(std::wstring_view extension) {
    while (!extension.empty() && extension.front() == L'.') {
        extension.remove_prefix(1);
    }

    std::wstring normalized(extension);
    for (auto& ch : normalized) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return normalized;
}

inline std::wstring BuildProgId(std::wstring_view extension) {
    return L"JarkViewer.ImageFile." + NormalizeExtension(extension);
}

inline std::wstring BuildTypeName(std::wstring_view extension) {
    auto normalized = NormalizeExtension(extension);
    return normalized.empty() ? L"JarkViewer 图像" : L"JarkViewer " + normalized + L" 图像";
}

inline std::wstring BuildExtensionKey(std::wstring_view extension) {
    return L"Software\\Classes\\." + NormalizeExtension(extension);
}

inline std::wstring BuildProgIdKey(std::wstring_view extension) {
    return L"Software\\Classes\\" + BuildProgId(extension);
}
}
