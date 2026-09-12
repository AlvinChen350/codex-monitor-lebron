#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <vterm.h>

inline std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline void utf8(std::string& s, uint32_t c) {
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) {
        s += static_cast<char>(0xc0 | (c >> 6));
        s += static_cast<char>(0x80 | (c & 63));
    } else if (c < 0x10000) {
        s += static_cast<char>(0xe0 | (c >> 12));
        s += static_cast<char>(0x80 | ((c >> 6) & 63));
        s += static_cast<char>(0x80 | (c & 63));
    } else {
        s += static_cast<char>(0xf0 | (c >> 18));
        s += static_cast<char>(0x80 | ((c >> 12) & 63));
        s += static_cast<char>(0x80 | ((c >> 6) & 63));
        s += static_cast<char>(0x80 | (c & 63));
    }
}

class Terminal {
    VTerm* terminal_;
    VTermScreen* screen_;
    int rows_, cols_;
    std::string replies_;
public:
    explicit Terminal(int rows = 100, int cols = 140)
        : terminal_(vterm_new(rows, cols)), rows_(rows), cols_(cols) {
        if (!terminal_) throw std::runtime_error("Cannot allocate terminal");
        vterm_set_utf8(terminal_, 1);
        vterm_output_set_callback(terminal_, [](const char* s, size_t n, void* p) {
            static_cast<Terminal*>(p)->replies_.append(s, n);
        }, this);
        screen_ = vterm_obtain_screen(terminal_);
        vterm_screen_enable_altscreen(screen_, 1);
        vterm_screen_reset(screen_, 1);
    }
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    ~Terminal() { vterm_free(terminal_); }
    void feed(const std::string& bytes) {
        vterm_input_write(terminal_, bytes.data(), bytes.size());
        vterm_screen_flush_damage(screen_);
    }
    std::string replies() {
        auto result = std::move(replies_);
        replies_.clear();
        return result;
    }
    std::string text() const {
        std::string result;
        for (int row = 0; row < rows_; ++row) {
            std::string line;
            for (int col = 0; col < cols_; ++col) {
                VTermScreenCell cell{};
                if (!vterm_screen_get_cell(screen_, VTermPos{row, col}, &cell)) continue;
                if (cell.chars[0] == UINT32_MAX) continue;
                if (!cell.chars[0]) line += ' ';
                else for (auto c : cell.chars) {
                    if (!c) break;
                    if (c != UINT32_MAX) utf8(line, c);
                }
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            result += line + '\n';
        }
        return result;
    }
};

inline std::string lastCard(const std::string& screen) {
    std::istringstream input(screen);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) lines.push_back(line);
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto col = lines[i].find("╭");
        if (col == std::string::npos || lines[i].find("╮", col) == std::string::npos) continue;
        std::string candidate;
        for (size_t j = i; j < lines.size(); ++j) {
            if (j > i && lines[j].find("╭") != std::string::npos) break;
            candidate += (lines[j].size() >= col ? lines[j].substr(col) : "") + '\n';
            if (lines[j].find("╰") == col && lines[j].find("╯", col) != std::string::npos) {
                const auto folded = lower(candidate);
                if (folded.find("5h limit:") != std::string::npos ||
                    folded.find("weekly limit:") != std::string::npos ||
                    (folded.find("model:") != std::string::npos &&
                     folded.find("account:") != std::string::npos)) result = candidate;
                break;
            }
        }
    }
    return result;
}

inline std::string statusLine(const std::string& card, const std::string& label) {
    std::istringstream input(card);
    for (std::string line; std::getline(input, line);)
        if (lower(line).find(lower(label)) != std::string::npos) return line;
    return {};
}

inline std::optional<int> percent(const std::string& card, const std::string& label) {
    auto line = statusLine(card, label);
    std::smatch match;
    if (!std::regex_search(line, match, std::regex(R"((\d+)%\s+left)"))) return {};
    try {
        int value = std::stoi(match[1]);
        if (value >= 0 && value <= 100) return value;
    } catch (...) {}
    return {};
}

inline std::tm localTime(std::time_t t) {
    std::tm value{};
#ifdef _WIN32
    localtime_s(&value, &t);
#else
    localtime_r(&t, &value);
#endif
    return value;
}

inline std::string timestamp(std::time_t t) {
    auto value = localTime(t);
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

inline std::optional<std::time_t> resetTime(const std::string& card, std::time_t now) {
    auto line = statusLine(card, "5h limit:");
    auto start = lower(line).find("resets ");
    if (start == std::string::npos) return {};
    std::string text = trim(line.substr(start + 7));
    auto end = text.find(')');
    if (end != std::string::npos) text.resize(end);
    text = trim(text);
    std::smatch m;
    static const std::regex pattern(
        R"(^(\d{1,2}):(\d{2})(?:\s+(AM|PM))?(?:\s+on\s+(\d{1,2})\s+([A-Za-z]{3})(?:\s+(\d{4}))?)?$)",
        std::regex::icase);
    if (!std::regex_match(text, m, pattern)) return {};
    auto candidate = localTime(now);
    int hour = std::stoi(m[1]), minute = std::stoi(m[2]);
    if (minute > 59) return {};
    if (m[3].matched) {
        if (hour < 1 || hour > 12) return {};
        hour = hour % 12 + (lower(m[3]) == "pm" ? 12 : 0);
    } else if (hour > 23) return {};
    candidate.tm_hour = hour;
    candidate.tm_min = minute;
    candidate.tm_sec = 0;
    candidate.tm_isdst = -1;
    if (m[4].matched) {
        const std::string months = "janfebmaraprmayjunjulaugsepoctnovdec";
        auto pos = months.find(lower(m[5]));
        int day = std::stoi(m[4]);
        if (pos == std::string::npos || pos % 3 || day < 1 || day > 31) return {};
        candidate.tm_mon = static_cast<int>(pos / 3);
        candidate.tm_mday = day;
        if (m[6].matched) candidate.tm_year = std::stoi(m[6]) - 1900;
        auto trial = candidate;
        auto t = std::mktime(&trial);
        if (!m[6].matched) {
            if (std::difftime(t, now) < -183.0 * 86400) ++candidate.tm_year;
            else if (std::difftime(t, now) > 183.0 * 86400) --candidate.tm_year;
        }
    }
    auto requested = candidate;
    auto t = std::mktime(&candidate);
    if (t == -1 || candidate.tm_mon != requested.tm_mon || candidate.tm_mday != requested.tm_mday ||
        candidate.tm_hour != hour || candidate.tm_min != minute) return {};
    if (!m[4].matched && std::difftime(t, now) < -300) {
        ++candidate.tm_mday;
        candidate.tm_isdst = -1;
        t = std::mktime(&candidate);
    }
    return t;
}

inline bool dormant(const std::string& card, std::time_t now) {
    auto remaining = percent(card, "5h limit:");
    auto reset = resetTime(card, now);
    if (!remaining || *remaining != 100 || !reset) return false;
    double seconds = std::difftime(*reset, now);
    return seconds >= 295 * 60 && seconds <= 305 * 60;
}

inline std::string jsonString(const std::string& value) {
    std::ostringstream result;
    result << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') result << '\\' << c;
        else if (c < 32) result << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c);
        else result << c;
    }
    result << '"';
    return result.str();
}

inline std::string safeName(std::string name) {
    for (auto& c : name)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '-' && c != '_') c = '_';
    return name.empty() ? "account" : name;
}
