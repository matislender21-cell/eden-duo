// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace Core::Mods::InputTokens {

struct StickPosition {
    bool right{};
    float x{};
    float y{};
};

inline std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

inline bool IsAxis(std::string_view text) {
    return text.size() == 3 && (text[0] == 'L' || text[0] == 'R') &&
           (text[1] == 'X' || text[1] == 'Y') && (text[2] == '+' || text[2] == '-');
}

inline std::optional<StickPosition> ParseStick(std::string_view text) {
    text = Trim(text);
    bool right{};
    double x{}, y{};
    if (IsAxis(text)) {
        right = text[0] == 'R';
        const double value = text[2] == '+' ? 1.0 : -1.0;
        (text[1] == 'X' ? x : y) = value;
    } else if (text.size() == 6 && IsAxis(text.substr(0, 3)) && IsAxis(text.substr(3, 3)) &&
               text[0] == text[3] && text[1] != text[4]) {
        // Contributor diagonal spelling, either X/Y order; generalized to the same vector path.
        right = text[0] == 'R';
        (text[1] == 'X' ? x : y) = text[2] == '+' ? 1.0 : -1.0;
        (text[4] == 'X' ? x : y) = text[5] == '+' ? 1.0 : -1.0;
    } else if ((text.starts_with("LStick(") || text.starts_with("RStick(")) &&
               text.ends_with(')')) {
        right = text[0] == 'R';
        const auto values = text.substr(7, text.size() - 8);
        const auto comma = values.find(',');
        if (comma == std::string_view::npos)
            return std::nullopt;
        const auto number = [](std::string_view value, double& result) {
            value = Trim(value);
            if (value.starts_with('+')) {
                value.remove_prefix(1);
                if (value.empty() || value.front() == '+' || value.front() == '-')
                    return false;
            }
            if (value.empty())
                return false;
            // Floating-point from_chars is unavailable in the supported Android libc++.
            // Classic locale keeps the manifest's decimal syntax independent of system locale.
            std::istringstream stream{std::string{value}};
            stream.imbue(std::locale::classic());
            stream >> std::noskipws >> result;
            return stream && stream.eof() && std::isfinite(result);
        };
        if (!number(values.substr(0, comma), x) || !number(values.substr(comma + 1), y)) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    // Preserve analog strength inside the disk; clamp outside it without changing direction.
    // Scale before hypot so even very large finite inputs cannot overflow.
    const double scale = std::max(std::abs(x), std::abs(y));
    if (scale > 1.0) {
        x /= scale;
        y /= scale;
        const double length = std::hypot(x, y);
        x /= length;
        y /= length;
    } else if (const double length = std::hypot(x, y); length > 1.0) {
        x /= length;
        y /= length;
    }
    return StickPosition{right, static_cast<float>(x), static_cast<float>(y)};
}

struct Token {
    std::string_view name;
    std::optional<StickPosition> stick;
};

/** Validate the whole chord before injecting anything. '+' axis signs are not separators. */
template <typename ButtonValidator>
std::optional<std::vector<Token>> ParseChord(std::string_view text, ButtonValidator valid_button) {
    std::vector<Token> tokens;
    bool sticks[2]{};
    std::size_t begin{};
    int depth{};
    const auto append = [&](std::string_view part) {
        part = Trim(part);
        if (part.empty())
            return false;
        auto stick = ParseStick(part);
        if (stick) {
            if (sticks[stick->right])
                return false; // Conflicting writes to one stick are ambiguous.
            sticks[stick->right] = true;
        } else if (!valid_button(part)) {
            return false;
        }
        tokens.push_back({part, stick});
        return true;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') {
            if (++depth > 1)
                return std::nullopt;
        } else if (text[i] == ')') {
            if (--depth < 0)
                return std::nullopt;
        } else if (text[i] == '+' && depth == 0) {
            if (i >= 2 && IsAxis(text.substr(i - 2, 3)))
                continue;
            if (!append(text.substr(begin, i - begin)))
                return std::nullopt;
            begin = i + 1;
        }
    }
    if (depth != 0 || !append(text.substr(begin)))
        return std::nullopt;
    return tokens;
}

} // namespace Core::Mods::InputTokens
