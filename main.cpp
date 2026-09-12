#include <windows.h>
#include <winhttp.h>
#include <objidl.h>
#include <gdiplus.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include "core.hpp"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
static std::atomic<bool> stopped{false};

static BOOL WINAPI onControl(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        stopped = true;
        return TRUE;
    }
    return FALSE;
}

static void check(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")");
}

static std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    check(n > 0, "Invalid UTF-8");
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), n);
    return result;
}

static std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    check(n > 0, "Cannot encode text");
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), n, nullptr, nullptr);
    return result;
}

static std::wstring env(const std::wstring& key) {
    DWORD n = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!n) return {};
    std::wstring result(n, L'\0');
    DWORD got = GetEnvironmentVariableW(key.c_str(), result.data(), n);
    check(got < n, "Environment changed while reading");
    result.resize(got);
    return result;
}

static std::wstring expand(const std::wstring& value) {
    DWORD n = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    check(n != 0, "Cannot expand path");
    std::wstring result(n, L'\0');
    check(ExpandEnvironmentStringsW(value.c_str(), result.data(), n) == n, "Cannot expand path");
    result.resize(n - 1);
    return result;
}

static std::wstring quote(const std::wstring& s) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : value(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE v = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = v;
    }
};

struct Internet {
    HINTERNET value;
    explicit Internet(HINTERNET v) : value(v) { check(v != nullptr, "HTTP operation failed"); }
    Internet(const Internet&) = delete;
    ~Internet() { WinHttpCloseHandle(value); }
};

struct Account {
    std::string name;
    std::wstring executable = L"codex";
    std::wstring home;
    fs::path directory;
};

struct Settings {
    int interval = 30, startup = 60, status = 30, model = 90, overall = 300, pause = 4;
    bool anchor = true;
    std::string webhookEnv = "DISCORD_WEBHOOK_URL";
    std::vector<Account> accounts;
    fs::path reports;
};

static int number(const std::string& value, int min, int max) {
    size_t end = 0;
    int n = std::stoi(value, &end);
    if (end != value.size() || n < min || n > max) throw std::runtime_error("Invalid setting: " + value);
    return n;
}

static Settings readConfig(const fs::path& filename) {
    std::ifstream input(filename);
    if (!input) throw std::runtime_error("Cannot open configuration");
    Settings s;
    auto base = fs::absolute(filename).parent_path();
    s.reports = base / L"reports";
    std::string section;
    std::map<std::string, bool> seen;
    std::map<std::string, bool> sections;
    for (std::string line; std::getline(input, line);) {
        if (line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            if (sections[section]) throw std::runtime_error("Duplicate section: " + section);
            sections[section] = true;
            if (section.rfind("account:", 0) == 0) {
                auto name = trim(section.substr(8));
                if (name.empty() || name.size() > 80) throw std::runtime_error("Invalid account name");
                Account a;
                a.name = name;
                a.directory = base;
                s.accounts.push_back(a);
            } else if (section != "monitor") throw std::runtime_error("Unknown section: " + section);
            continue;
        }
        auto equal = line.find('=');
        if (equal == std::string::npos) throw std::runtime_error("Expected key=value");
        auto key = trim(line.substr(0, equal));
        auto value = trim(line.substr(equal + 1));
        auto id = section + "/" + key;
        if (seen[id]) throw std::runtime_error("Duplicate setting: " + id);
        seen[id] = true;
        if (section == "monitor") {
            if (key == "interval_minutes") s.interval = number(value, 1, 60);
            else if (key == "startup_timeout_seconds") s.startup = number(value, 5, 300);
            else if (key == "status_timeout_seconds") s.status = number(value, 5, 300);
            else if (key == "model_timeout_seconds") s.model = number(value, 5, 600);
            else if (key == "overall_timeout_seconds") s.overall = number(value, 30, 1800);
            else if (key == "refresh_pause_seconds") s.pause = number(value, 1, 30);
            else if (key == "webhook_env") s.webhookEnv = value;
            else if (key == "auto_anchor") {
                if (value != "true" && value != "false") throw std::runtime_error("auto_anchor must be true or false");
                s.anchor = value == "true";
            } else throw std::runtime_error("Unknown setting: " + key);
        } else if (section.rfind("account:", 0) == 0 && !s.accounts.empty()) {
            auto& a = s.accounts.back();
            if (key == "executable") a.executable = expand(wide(value));
            else if (key == "codex_home") {
                if (!value.empty()) {
                    fs::path p(expand(wide(value)));
                    a.home = fs::absolute(p.is_absolute() ? p : base / p).wstring();
                }
            } else if (key == "working_directory") {
                fs::path p(expand(wide(value)));
                a.directory = fs::absolute(p.is_absolute() ? p : base / p);
            } else throw std::runtime_error("Unknown account setting: " + key);
        } else throw std::runtime_error("Setting outside a section");
    }
    if (s.accounts.empty()) throw std::runtime_error("No accounts configured");
    if (60 % s.interval) throw std::runtime_error("interval_minutes must divide 60");
    if (s.webhookEnv.empty()) throw std::runtime_error("webhook_env is empty");
    for (auto& a : s.accounts) {
        if (a.executable.empty()) throw std::runtime_error("Codex executable is empty");
        if (!fs::is_directory(a.directory)) throw std::runtime_error("Working directory does not exist: " + a.name);
        if (!a.home.empty() && !fs::is_directory(a.home)) throw std::runtime_error("CODEX_HOME does not exist: " + a.name);
    }
    return s;
}

struct EnvironmentLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

static std::vector<wchar_t> childEnvironment(const Account& a) {
    std::map<std::wstring, std::wstring, EnvironmentLess> values;
    wchar_t* block = GetEnvironmentStringsW();
    check(block != nullptr, "Cannot read environment");
    for (auto p = block; *p; p += wcslen(p) + 1) {
        std::wstring entry(p);
        auto equal = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
        if (equal != std::wstring::npos) values[entry.substr(0, equal)] = entry.substr(equal + 1);
    }
    FreeEnvironmentStringsW(block);
    if (!a.home.empty()) values[L"CODEX_HOME"] = a.home;
    values[L"TERM"] = L"xterm-256color";
    std::vector<wchar_t> result;
    for (auto& [key, value] : values) {
        auto entry = key + L"=" + value;
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(0);
    }
    result.push_back(0);
    return result;
}

static std::wstring executablePath(const std::wstring& name) {
    for (auto suffix : {L".exe", L".cmd", L".bat"}) {
        std::vector<wchar_t> buffer(32768);
        DWORD n = SearchPathW(nullptr, name.c_str(), suffix, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (n && n < buffer.size()) return std::wstring(buffer.data(), n);
    }
    throw std::runtime_error("Codex was not found. Set executable to its full path.");
}

struct Snapshot {
    std::string text;
    uint64_t revision;
    Clock::time_point changed;
};

class Session {
    Terminal terminal_;
    Handle input_, output_, process_, job_;
    HPCON console_ = nullptr;
    std::mutex terminalMutex_, inputMutex_;
    std::thread reader_;
    std::atomic<bool> closing_{false}, ended_{false};
    uint64_t revision_ = 0;
    Clock::time_point changed_ = Clock::now();
    Clock::time_point overall_;
    bool debug_;

    void readLoop() {
        try {
            while (!closing_) {
                DWORD available = 0;
                if (!PeekNamedPipe(output_.value, nullptr, 0, nullptr, &available, nullptr)) break;
                if (!available) { std::this_thread::sleep_for(10ms); continue; }
                char buffer[16384];
                DWORD read = 0;
                if (!ReadFile(output_.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || !read) break;
                std::string response;
                {
                    std::lock_guard<std::mutex> lock(terminalMutex_);
                    terminal_.feed(std::string(buffer, read));
                    response = terminal_.replies();
                    ++revision_;
                    changed_ = Clock::now();
                }
                if (!response.empty()) send(response);
            }
        } catch (...) {}
        ended_ = true;
    }

    void shutdown() noexcept {
        if (job_.value) TerminateJobObject(job_.value, 0);
        if (console_) { ClosePseudoConsole(console_); console_ = nullptr; }
        closing_ = true;
        if (reader_.joinable()) reader_.join();
    }

public:
    Session(const Account& a, const Settings& s, bool debug)
        : overall_(Clock::now() + std::chrono::seconds(s.overall)), debug_(debug) {
        try {
            Handle inputRead, outputWrite;
            check(CreatePipe(&inputRead.value, &input_.value, nullptr, 0), "Cannot create input pipe");
            check(CreatePipe(&output_.value, &outputWrite.value, nullptr, 0), "Cannot create output pipe");
            HRESULT hr = CreatePseudoConsole(COORD{140, 100}, inputRead.value, outputWrite.value, 0, &console_);
            if (FAILED(hr)) throw std::runtime_error("Cannot create ConPTY; Windows 10 1809 or newer is required");
            reader_ = std::thread([this] { readLoop(); });
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            std::vector<unsigned char> storage(size);
            auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            check(InitializeProcThreadAttributeList(attrs, 1, 0, &size), "Cannot initialize process attributes");
            struct AttributesGuard {
                LPPROC_THREAD_ATTRIBUTE_LIST p;
                ~AttributesGuard() { DeleteProcThreadAttributeList(p); }
            } guard{attrs};
            check(UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                console_, sizeof(console_), nullptr, nullptr), "Cannot attach ConPTY");
            job_.reset(CreateJobObjectW(nullptr, nullptr));
            check(job_.value != nullptr, "Cannot create process job");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            check(SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Cannot configure process job");
            auto executable = executablePath(a.executable);
            auto command = quote(executable) + L" -c check_for_update_on_startup=false -a never -s read-only";
            auto extension = lower(narrow(fs::path(executable).extension().wstring()));
            if (extension == ".cmd" || extension == ".bat") {
                if (executable.find_first_of(L"\"%\r\n") != std::wstring::npos)
                    throw std::runtime_error("Unsupported characters in Codex launcher path; use codex.exe directly");
                wchar_t system[MAX_PATH];
                check(GetSystemDirectoryW(system, MAX_PATH) != 0, "Cannot locate system directory");
                executable = (fs::path(system) / L"cmd.exe").wstring();
                command = quote(executable) + L" /d /v:off /s /c \"" + command + L"\"";
            }
            auto environment = childEnvironment(a);
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.lpAttributeList = attrs;
            PROCESS_INFORMATION info{};
            check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                environment.data(), a.directory.c_str(), &startup.StartupInfo, &info), "Cannot start Codex");
            process_.reset(info.hProcess);
            Handle thread(info.hThread);
            if (!AssignProcessToJobObject(job_.value, process_.value)) {
                TerminateProcess(process_.value, 1);
                throw std::runtime_error("Cannot isolate Codex process");
            }
            check(ResumeThread(thread.value) != static_cast<DWORD>(-1), "Cannot resume Codex process");
            inputRead.reset();
            outputWrite.reset();
        } catch (...) { shutdown(); throw; }
    }

    ~Session() { shutdown(); }

    void send(const std::string& text) {
        std::lock_guard<std::mutex> lock(inputMutex_);
        size_t offset = 0;
        while (offset < text.size()) {
            DWORD written = 0;
            check(WriteFile(input_.value, text.data() + offset, static_cast<DWORD>(text.size() - offset), &written, nullptr) && written,
                "Cannot write to Codex");
            offset += written;
        }
    }

    Snapshot snapshot() {
        if (stopped) throw std::runtime_error("Stopped");
        if (Clock::now() >= overall_) throw std::runtime_error("Account timed out");
        if (ended_ || WaitForSingleObject(process_.value, 0) == WAIT_OBJECT_0)
            throw std::runtime_error("Codex exited before completing the check");
        std::lock_guard<std::mutex> lock(terminalMutex_);
        return {terminal_.text(), revision_, changed_};
    }

    template<class Predicate>
    Snapshot wait(int seconds, const std::string& operation, Predicate predicate) {
        auto deadline = std::min(overall_, Clock::now() + std::chrono::seconds(seconds));
        for (;;) {
            auto current = snapshot();
            if (predicate(current)) return current;
            if (Clock::now() >= deadline) {
                if (debug_) std::cerr << current.text << '\n';
                throw std::runtime_error(operation + " timed out. Run Codex manually in working_directory to finish login/setup, or use --debug.");
            }
            std::this_thread::sleep_for(50ms);
        }
    }

    static bool ready(const Snapshot& s) {
        return s.text.find("Ask Codex to do anything") != std::string::npos;
    }

    void ready(int seconds) {
        wait(seconds, "Codex startup", [](const Snapshot& s) {
            return ready(s) && Clock::now() - s.changed >= 750ms;
        });
    }

    void type(const std::string& text) {
        for (char c : text) {
            snapshot();
            send(std::string(1, c));
            std::this_thread::sleep_for(20ms);
        }
    }

    void pause(int seconds) {
        auto until = Clock::now() + std::chrono::seconds(seconds);
        while (Clock::now() < until) { snapshot(); std::this_thread::sleep_for(50ms); }
    }

    std::string status(int seconds) {
        type("/status");
        wait(seconds, "Status input", [](const Snapshot& s) {
            return s.text.find("show current session configuration and token usage") != std::string::npos &&
                Clock::now() - s.changed >= 100ms;
        });
        auto revision = snapshot().revision;
        auto sent = Clock::now();
        send("\r");
        auto result = wait(seconds, "Status capture", [&](const Snapshot& s) {
            return s.revision > revision && ready(s) &&
                s.text.find("show current session configuration and token usage") == std::string::npos &&
                Clock::now() - sent >= 1s && Clock::now() - s.changed >= 750ms &&
                !lastCard(s.text).empty();
        });
        return lastCard(result.text);
    }

    void anchor(int seconds) {
        const std::string prompt = "Reply only OK. Do not use tools.";
        type(prompt);
        wait(seconds, "Anchor input", [&](const Snapshot& s) { return s.text.find(prompt) != std::string::npos; });
        auto revision = snapshot().revision;
        auto sent = Clock::now();
        send("\r");
        wait(seconds, "Anchor response", [&](const Snapshot& s) {
            auto folded = lower(s.text);
            if (folded.find("you've hit your usage limit") != std::string::npos ||
                folded.find("usage limit reached") != std::string::npos)
                throw std::runtime_error("Model request reached a usage limit");
            bool ok = false;
            std::istringstream lines(s.text);
            for (std::string line; std::getline(lines, line);) {
                line = trim(line);
                if (line == "OK" || line == "• OK" || line == "● OK") ok = true;
            }
            return s.revision > revision && ready(s) && ok &&
                folded.find("esc to interrupt") == std::string::npos &&
                Clock::now() - sent >= 2s && Clock::now() - s.changed >= 1s;
        });
    }
};

static std::string capture(Session& session, const Settings& settings, const std::string& name) {
    std::string card;
    for (int i = 1; i <= 3; ++i) {
        std::cout << '[' << name << "] /status " << i << "/3\n";
        card = session.status(settings.status);
        if (i < 3) session.pause(settings.pause);
    }
    return card;
}

static std::vector<unsigned char> renderPng(const std::string& text) {
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
        throw std::runtime_error("Cannot start image renderer");
    struct Guard { ULONG_PTR token; ~Guard() { Gdiplus::GdiplusShutdown(token); } } guard{token};
    auto content = wide(text);
    Gdiplus::Font font(L"Consolas", 18.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    if (font.GetLastStatus() != Gdiplus::Ok) throw std::runtime_error("Consolas font is unavailable");
    Gdiplus::Bitmap measure(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics metrics(&measure);
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF bounds;
    metrics.MeasureString(content.c_str(), static_cast<INT>(content.size()), &font,
        Gdiplus::PointF(0, 0), &format, &bounds);
    int width = static_cast<int>(bounds.Width + 56), height = static_cast<int>(bounds.Height + 56);
    if (width < 1 || height < 1 || width > 6000 || height > 6000) throw std::runtime_error("Status image is too large");
    Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&bitmap);
    graphics.Clear(Gdiplus::Color(255, 22, 25, 31));
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 230, 234, 241));
    if (graphics.DrawString(content.c_str(), static_cast<INT>(content.size()), &font,
        Gdiplus::PointF(28, 24), &format, &brush) != Gdiplus::Ok)
        throw std::runtime_error("Cannot render status image");
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    if (!bytes) throw std::runtime_error("PNG encoder is unavailable");
    std::vector<unsigned char> storage(bytes);
    auto encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    Gdiplus::GetImageEncoders(count, bytes, encoders);
    CLSID encoder{};
    bool found = false;
    for (UINT i = 0; i < count; ++i)
        if (wcscmp(encoders[i].MimeType, L"image/png") == 0) { encoder = encoders[i].Clsid; found = true; break; }
    if (!found) throw std::runtime_error("PNG encoder is unavailable");
    IStream* raw = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &raw))) throw std::runtime_error("Cannot allocate PNG stream");
    struct StreamGuard { IStream* p; ~StreamGuard() { p->Release(); } } stream{raw};
    if (bitmap.Save(raw, &encoder, nullptr) != Gdiplus::Ok) throw std::runtime_error("Cannot encode PNG");
    STATSTG stat{};
    if (FAILED(raw->Stat(&stat, STATFLAG_NONAME))) throw std::runtime_error("Cannot read PNG size");
    LARGE_INTEGER zero{};
    if (FAILED(raw->Seek(zero, STREAM_SEEK_SET, nullptr))) throw std::runtime_error("Cannot read PNG stream");
    std::vector<unsigned char> result(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG got = 0;
    if (FAILED(raw->Read(result.data(), static_cast<ULONG>(result.size()), &got)) || got != result.size())
        throw std::runtime_error("Cannot read PNG data");
    return result;
}

static void validateWebhook(const std::wstring& url) {
    static const std::wregex pattern(LR"(^https://(?:discord\.com|canary\.discord\.com|ptb\.discord\.com)/api/webhooks/[0-9]+/[A-Za-z0-9_-]+$)");
    if (!std::regex_match(url, pattern)) throw std::runtime_error("Set DISCORD_WEBHOOK_URL to a Discord channel webhook URL");
}

static void discord(const std::wstring& webhook, const std::string& title, const std::string& report,
                    const std::vector<unsigned char>& png) {
    const std::string boundary = "CodexMonitor" + std::to_string(GetTickCount64());
    std::string payload = "{\"content\":" + jsonString(title) + ",\"allowed_mentions\":{\"parse\":[]}";
    if (!png.empty()) payload += ",\"embeds\":[{\"image\":{\"url\":\"attachment://status.png\"}}]";
    payload += '}';
    std::string body;
    auto part = [&](const std::string& disposition, const std::string& type, const std::string& bytes) {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; " + disposition +
            "\r\nContent-Type: " + type + "\r\n\r\n" + bytes + "\r\n";
    };
    part("name=\"payload_json\"", "application/json", payload);
    part("name=\"files[0]\"; filename=\"status.txt\"", "text/plain; charset=utf-8", report);
    if (!png.empty()) part("name=\"files[1]\"; filename=\"status.png\"", "image/png",
        std::string(reinterpret_cast<const char*>(png.data()), png.size()));
    body += "--" + boundary + "--\r\n";
    auto url = webhook + L"?wait=true";
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = components.dwUrlPathLength = components.dwExtraInfoLength = static_cast<DWORD>(-1);
    check(WinHttpCrackUrl(url.c_str(), 0, 0, &components), "Cannot parse webhook URL");
    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    Internet session(WinHttpOpen(L"CodexMonitor/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    check(WinHttpSetTimeouts(session.value, 20000, 20000, 20000, 20000), "Cannot set HTTP timeout");
    Internet connection(WinHttpConnect(session.value, host.c_str(), components.nPort, 0));
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (stopped) throw std::runtime_error("Stopped");
        Internet request(WinHttpOpenRequest(connection.value, L"POST", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        check(WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)), "Cannot configure HTTP request");
        std::wstring headers = L"Content-Type: multipart/form-data; boundary=" + wide(boundary) + L"\r\n";
        check(WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(-1), body.data(),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0), "Discord upload failed");
        check(WinHttpReceiveResponse(request.value, nullptr), "Discord response failed");
        DWORD code = 0, size = sizeof(code);
        check(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX), "Cannot read Discord status");
        if (code >= 200 && code < 300) return;
        if (code != 429 || attempt == 2) throw std::runtime_error("Discord returned HTTP " + std::to_string(code));
        wchar_t retry[128]{};
        size = sizeof(retry);
        double seconds = 5;
        if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CUSTOM, L"Retry-After", retry, &size, WINHTTP_NO_HEADER_INDEX)) {
            try { seconds = std::stod(retry); } catch (...) {}
        }
        if (!(seconds >= 0 && seconds <= 60)) throw std::runtime_error("Discord rate limit exceeds 60 seconds; next cycle will retry");
        auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000) + 250);
        while (!stopped && Clock::now() < deadline) std::this_thread::sleep_for(50ms);
    }
}

static void writeFile(const fs::path& file, const char* bytes, size_t size) {
    std::ofstream out(file, std::ios::binary);
    if (!out || !out.write(bytes, static_cast<std::streamsize>(size))) throw std::runtime_error("Cannot save report");
}

static bool cycle(const Settings& settings, const std::wstring& webhook, bool local, bool debug) {
    bool success = true;
    fs::create_directories(settings.reports);
    for (size_t i = 0; i < settings.accounts.size() && !stopped; ++i) {
        const auto& account = settings.accounts[i];
        std::string report, title = "Codex Usage Monitor - " + account.name;
        try {
            std::cout << '[' << account.name << "] starting Codex\n";
            Session session(account, settings, debug);
            session.ready(settings.startup);
            auto card = capture(session, settings, account.name);
            std::string note;
            if (settings.anchor && dormant(card, std::time(nullptr))) {
                std::cout << '[' << account.name << "] sending anchor request\n";
                try {
                    session.anchor(settings.model);
                    card = capture(session, settings, account.name);
                    note = "Anchor request completed; status refreshed.\n";
                } catch (const std::exception& e) {
                    if (stopped) break;
                    success = false;
                    note = "Anchor check failed. Showing the pre-anchor status.\n" + std::string(e.what()) + "\n";
                }
            }
            report = title + "\n" + timestamp(std::time(nullptr)) + " (local time)\n" + note + "\n" + card;
        } catch (const std::exception& e) {
            if (stopped) break;
            success = false;
            report = title + "\n" + timestamp(std::time(nullptr)) + "\nCheck failed: " + e.what() + "\n";
        }
        std::cout << report << '\n';
        auto stem = std::to_string(i + 1) + "-" + safeName(account.name);
        std::vector<unsigned char> png;
        try {
            writeFile(settings.reports / (stem + ".txt"), report.data(), report.size());
            png = renderPng(report);
            writeFile(settings.reports / (stem + ".png"), reinterpret_cast<const char*>(png.data()), png.size());
        } catch (const std::exception& e) {
            success = false;
            std::cerr << '[' << account.name << "] " << e.what() << '\n';
        }
        if (!local && !stopped) {
            try {
                discord(webhook, title, report, png);
                std::cout << '[' << account.name << "] Discord report sent\n";
            } catch (const std::exception& e) {
                success = false;
                std::cerr << '[' << account.name << "] " << e.what() << '\n';
            }
        }
    }
    return success;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onControl, TRUE);
    try {
        fs::path config = L"config.ini";
        bool once = false, local = false, debug = false, noAnchor = false;
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--once") once = true;
            else if (arg == L"--no-discord") local = true;
            else if (arg == L"--no-anchor") noAnchor = true;
            else if (arg == L"--debug") debug = true;
            else if (arg == L"--config" && i + 1 < argc) config = argv[++i];
            else if (arg == L"--help" || arg == L"-h") {
                std::cout << "codex-monitor [--config FILE] [--once] [--no-discord] [--no-anchor] [--debug]\n";
                return 0;
            } else throw std::runtime_error("Unknown or incomplete argument");
        }
        auto settings = readConfig(config);
        if (noAnchor) settings.anchor = false;
        std::wstring webhook;
        if (!local) {
            webhook = env(wide(settings.webhookEnv));
            validateWebhook(webhook);
        }
        Handle lock(CreateMutexW(nullptr, FALSE, L"Local\\CodexMonitorCpp"));
        check(lock.value != nullptr, "Cannot create monitor lock");
        if (GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("Another monitor instance is already running");
        bool ok = cycle(settings, webhook, local, debug);
        if (once || stopped) return ok ? 0 : 1;
        while (!stopped) {
            auto now = std::time(nullptr);
            auto next = localTime(now);
            next.tm_min = (next.tm_min / settings.interval + 1) * settings.interval;
            next.tm_sec = 0;
            next.tm_isdst = -1;
            auto deadline = std::mktime(&next);
            if (deadline <= now) deadline = now + settings.interval * 60;
            std::cout << "Next check: " << timestamp(deadline) << '\n';
            while (!stopped && std::time(nullptr) < deadline) std::this_thread::sleep_for(250ms);
            if (!stopped) cycle(settings, webhook, local, debug);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
