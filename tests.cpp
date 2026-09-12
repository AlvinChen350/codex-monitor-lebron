#include <cstdlib>
#include <iostream>
#include "core.hpp"

static int checks = 0;

static void expect(bool condition, const char* name) {
    ++checks;
    if (!condition) throw std::runtime_error(name);
}

static std::time_t date(int year, int month, int day, int hour, int minute = 0) {
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_isdst = -1;
    return std::mktime(&value);
}

static std::string card(const std::string& value, const std::string& reset) {
    return "╭────────────────────────────────────────────────────────────╮\n"
        "│ Model: test                                                │\n"
        "│ 5h limit: " + value + "% left (resets " + reset + ")\n"
        "│ Weekly limit: 72% left                                     │\n"
        "│ Future field: preserved                                    │\n"
        "╰────────────────────────────────────────────────────────────╯\n";
}

int main() {
    try {
        auto now = date(2026, 9, 12, 8);
        auto full = card("100", "13:00");
        expect(percent(full, "5h limit:") == 100, "100 percent is not zero");
        expect(percent(card("0", "13:00"), "5h limit:") == 0, "zero percent");
        expect(percent(full, "Weekly limit:") == 72, "weekly quota");
        expect(!percent(full, "Other limit:"), "missing quota");
        expect(!percent(card("101", "13:00"), "5h limit:"), "invalid percentage");
        expect(!percent(card("9999999999999999999", "13:00"), "5h limit:"), "percentage overflow");
        expect(resetTime(full, now) == date(2026, 9, 12, 13), "same-day reset");
        expect(resetTime(card("100", "1:00 PM"), now) == date(2026, 9, 12, 13), "12-hour reset");
        expect(resetTime(card("100", "03:00"), date(2026, 9, 12, 22)) == date(2026, 9, 13, 3), "midnight rollover");
        expect(resetTime(card("100", "03:00 on 1 Jan"), date(2026, 12, 31, 22)) == date(2027, 1, 1, 3), "year rollover");
        expect(resetTime(card("100", "03:00 on 1 Jan 2027"), now) == date(2027, 1, 1, 3), "explicit year");
        expect(!resetTime(card("100", "25:00"), now), "invalid hour");
        expect(!resetTime(card("100", "13:99"), now), "invalid minute");
        expect(!resetTime(card("100", "13:00 on 31 Feb"), now), "invalid date");
        expect(!resetTime(card("100", "unknown"), now), "unknown reset");
        expect(dormant(full, now), "dormant five-hour window");
        expect(!dormant(card("99", "13:00"), now), "active window");
        expect(!dormant(card("100", "12:00"), now), "full active window");
        expect(!dormant(card("100", "unknown"), now), "unknown state stays passive");
        expect(lastCard(full).find("Future field: preserved") != std::string::npos, "unknown fields preserved");
        expect(lastCard(full + card("25", "12:00")).find("25%") != std::string::npos, "newest card");
        expect(lastCard("╭────╮\n│login│\n╰────╯\n").empty(), "non-status card rejected");
        expect(lastCard(full + "╭────╮\n│incomplete\n") == full, "incomplete card ignored");
        expect(jsonString("a\n\"\\\t") == "\"a\\u000a\\\"\\\\\\u0009\"", "JSON escaping");
        expect(safeName("../../Main") == "______Main", "safe report filename");
        Terminal terminal(100, 140);
        terminal.feed("old output\x1b[2J\x1b[H");
        std::string terminalCard;
        for (char c : full) terminalCard += c == '\n' ? "\r\n" : std::string(1, c);
        for (char c : terminalCard) terminal.feed(std::string(1, c));
        auto rendered = terminal.text();
        expect(rendered.find("old output") == std::string::npos, "terminal erase");
        expect(lastCard(rendered) == full, "fragmented UTF-8 terminal capture");
        terminal.feed("\x1b[2J\x1b[Hhello\rX");
        expect(terminal.text().rfind("Xello", 0) == 0, "cursor overwrite");
        terminal.feed("\x1b[2J\x1b[H台灣\r\nOK");
        expect(terminal.text().rfind("台灣\nOK", 0) == 0, "wide Unicode cells");
        terminal.feed("\x1b[6n");
        expect(terminal.replies().find('R') != std::string::npos, "cursor position response");
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Failed: " << e.what() << '\n';
        return 1;
    }
}
