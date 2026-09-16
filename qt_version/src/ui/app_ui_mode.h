#pragma once
#include <cctype>
#include <string>

// Which UI layer the application presents.
//   Clinical — YCTS default: Record + Viewer tabs only, reduced control set,
//              light clinical theme.
//   Research — the full upstream Local Tracking Software tab set and the
//              dark "Clinical Slate" theme.
enum class UiMode { Clinical, Research };

// Parses "clinical" / "research" (case-insensitive). Anything else, including
// an empty string, yields the fallback.
inline UiMode parseUiMode(const std::string& value, UiMode fallback = UiMode::Clinical) {
    std::string v;
    v.reserve(value.size());
    for (char c : value) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "research" || v == "full") return UiMode::Research;
    if (v == "clinical") return UiMode::Clinical;
    return fallback;
}

inline const char* uiModeName(UiMode mode) {
    return mode == UiMode::Research ? "research" : "clinical";
}
