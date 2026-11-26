// flamingo-worker: helps a Flamingo Port build from another PC on the network. It takes shaders already translated
// to HLSL from the main machine (tools/dxil_cache.py --serve), compiles them with DXC and sends the DXIL back. DXC is
// most of the shader stage's time; the translation stays on the main machine, so a helper needs only this program,
// dxc.exe and dxcompiler.dll (the same version as the main machine's), and no copy of the game's data.
//
// Plain HTTP on the local network: the main machine accepts private addresses only, and every request carries the
// build's token. The address and the token come from worker.txt next to this program (lines server=http://IP:PORT
// and token=...; the launcher shows them) or from --server and --token. The helper keeps running: it waits for the
// main machine, helps while a build runs, and waits again when it's over.
//
//   GET  /job?w=<name>            200: X-Job-Id, X-Profile, X-Opt headers, the HLSL as the body;
//                                 204: nothing right now (ask again soon); 410: the build is over (exit)
//   POST /result?id=<id>&ok=1     the DXIL as the body;   ok=0: the error message
// Every request carries X-Protocol: 1 and X-Token; a main machine speaking another version answers 426, a wrong
// token 403.
//
// Build: scripts\build_worker.ps1 (MSVC, static CRT: nothing else to install on the helper).
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring g_headers;     // X-Protocol and X-Token

struct Options {
    std::wstring server, token;
    std::wstring dxc;
    int jobs = 0;
    int timeout = 900;
};

struct Url {
    std::wstring host, path;
    INTERNET_PORT port = 80;
    bool https = false;
};

std::atomic<int> g_ok{0}, g_failed{0}, g_active{0};
std::atomic<bool> g_over{false};
wchar_t g_name[MAX_COMPUTERNAME_LENGTH + 1];
CRITICAL_SECTION g_print;

void say(const char* fmt, ...) {
    EnterCriticalSection(&g_print);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::printf("%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
    LeaveCriticalSection(&g_print);
}

bool crack(const std::wstring& s, Url& u) {
    URL_COMPONENTS c{sizeof c};
    wchar_t host[256], path[1024];
    c.lpszHostName = host;
    c.dwHostNameLength = 256;
    c.lpszUrlPath = path;
    c.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(s.c_str(), 0, 0, &c)) return false;
    u.host.assign(host, c.dwHostNameLength);
    u.path.assign(path, c.dwUrlPathLength);
    while (!u.path.empty() && u.path.back() == L'/') u.path.pop_back();
    u.port = c.nPort;
    u.https = c.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

struct Reply {
    DWORD status = 0;
    std::string body;
    std::wstring job, profile, opt, timeout;
};

std::wstring header(HINTERNET req, const wchar_t* name) {
    wchar_t buf[256];
    DWORD size = sizeof buf;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, buf, &size, WINHTTP_NO_HEADER_INDEX)) return L"";
    return std::wstring(buf, size / sizeof(wchar_t));
}

// One request on a connection; false when the main machine can't be reached.
bool call(HINTERNET conn, const Url& u, const wchar_t* verb, const std::wstring& path, const std::string& body,
          Reply& r) {
    HINTERNET req = WinHttpOpenRequest(conn, verb, (u.path + path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return false;
    DWORD to = 120000;
    WinHttpSetTimeouts(req, to, to, to, to);
    bool ok = WinHttpSendRequest(req, g_headers.c_str(), (DWORD)-1L, body.empty() ? WINHTTP_NO_REQUEST_DATA
                                                                           : const_cast<char*>(body.data()),
                                 (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD size = sizeof r.status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &r.status, &size, WINHTTP_NO_HEADER_INDEX);
        r.job = header(req, L"X-Job-Id");
        r.profile = header(req, L"X-Profile");
        r.opt = header(req, L"X-Opt");
        r.timeout = header(req, L"X-Timeout");
        r.body.clear();
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
            size_t at = r.body.size();
            r.body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, r.body.data() + at, avail, &got)) {
                ok = false;
                break;
            }
            r.body.resize(at + got);
        }
    }
    WinHttpCloseHandle(req);
    return ok;
}

bool write_file(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    CloseHandle(f);
    return ok;
}

bool read_file(const std::wstring& path, std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    data.resize((size_t)size.QuadPart);
    DWORD got = 0;
    bool ok = ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size();
    CloseHandle(f);
    return ok;
}

// The first "error:" line of DXC's output, without the file and position before it.
std::string first_error(const std::string& out) {
    size_t at = out.find("error:");
    if (at == std::string::npos) {
        size_t s = out.find_first_not_of(" \r\n");
        return s == std::string::npos ? "dxc failed" : out.substr(s, out.find_first_of("\r\n", s) - s).substr(0, 160);
    }
    std::string line = out.substr(at + 6, out.find_first_of("\r\n", at) - at - 6);
    size_t s = line.find_first_not_of(' ');
    return s == std::string::npos ? "dxc failed" : line.substr(s, 160);
}

// Runs DXC on one HLSL file; true with the DXIL, false with the error.
bool compile(const Options& o, const std::wstring& dir, const Reply& job, std::string& result) {
    std::wstring hlsl = dir + L"\\shader.hlsl", dxil = dir + L"\\shader.dxil", log = dir + L"\\dxc.txt";
    DeleteFileW(dxil.c_str());
    if (!write_file(hlsl, job.body)) {
        result = "helper: can't write the HLSL";
        return false;
    }
    std::wstring cmd = L"\"" + o.dxc + L"\" -T " + job.profile + L" -E main " + (job.opt.empty() ? L"-O0" : job.opt) +
                       L" \"" + hlsl + L"\" -Fo \"" + dxil + L"\"";
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    // DXC inherits its output file and nothing else (not this program's own stdout, a log other processes open)
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attrs(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    InitializeProcThreadAttributeList(list, 1, 0, &size);
    UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof out, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = out;
    si.StartupInfo.hStdInput = nullptr;
    si.lpAttributeList = list;
    PROCESS_INFORMATION pi{};
    bool started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                  nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(list);
    CloseHandle(out);
    if (!started) {
        result = "helper: can't start dxc";
        return false;
    }
    DWORD code = 1;
    int seconds = job.timeout.empty() ? o.timeout : _wtoi(job.timeout.c_str());   // the main machine's limit
    if (seconds <= 0) seconds = o.timeout;
    if (WaitForSingleObject(pi.hProcess, (DWORD)seconds * 1000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        result = "dxc: timed out";
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return false;
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code == 0 && read_file(dxil, result) && result.size() >= 4 && result.compare(0, 4, "DXBC") == 0) return true;
    std::string text;
    read_file(log, text);
    result = "dxc: " + first_error(text);
    return false;
}

void work(const Options& o, const Url& u, int n) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"flamingo-worker";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(n);
    CreateDirectoryW(dir.c_str(), nullptr);

    HINTERNET s = WinHttpOpen(L"flamingo-worker/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = s ? WinHttpConnect(s, u.host.c_str(), u.port, 0) : nullptr;
    bool waiting = false;
    while (conn && !g_over) {
        Reply job;
        bool reached = call(conn, u, L"GET", L"/job?w=" + std::wstring(g_name), "", job);
        if (!reached || job.status == 410) {        // no build running there now: wait for the next one
            if (!waiting && n == 0) say("waiting for the main machine to prepare...");
            waiting = true;
            Sleep(5000);
            continue;
        }
        if (waiting && n == 0) say("helping");
        waiting = false;
        if (job.status == 403) {
            if (n == 0) say("the main machine refused the token: copy worker.txt again from the launcher");
            g_over = true;
            break;
        }
        if (job.status == 426) {
            if (n == 0) say("the main machine runs another version: update this helper");
            g_over = true;
            break;
        }
        if (job.status != 200 || job.job.empty() || job.profile.empty()) {
            Sleep(1000);
            continue;
        }
        ++g_active;
        std::string result;
        bool ok = compile(o, dir, job, result);
        --g_active;
        Reply done;
        std::wstring path = L"/result?id=" + job.job + L"&ok=" + (ok ? L"1" : L"0");
        for (int attempt = 0; attempt < 10 && !call(conn, u, L"POST", path, result, done); ++attempt) Sleep(2000);
        int k = ok ? ++g_ok : ++g_failed;
        if ((g_ok + g_failed) % 25 == 0 || !ok)
            say("%d compiled, %d failed%s%s", g_ok.load(), g_failed.load(), ok ? "" : "  (last: ", ok ? "" : (result + ")").c_str());
        (void)k;
    }
    if (conn) WinHttpCloseHandle(conn);
    if (s) WinHttpCloseHandle(s);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    InitializeCriticalSection(&g_print);
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(g_name, &len);
    Options o;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring here(self);
    here = here.substr(0, here.find_last_of(L"\\/"));
    o.dxc = here + L"\\dxc.exe";
    // worker.txt next to the program: server=... and token=...
    if (FILE* f = _wfopen((here + L"\\worker.txt").c_str(), L"r, ccs=UTF-8")) {
        wchar_t line[512];
        while (fgetws(line, 512, f)) {
            std::wstring s = line;
            while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
            if (s.rfind(L"server=", 0) == 0) o.server = s.substr(7);
            else if (s.rfind(L"token=", 0) == 0) o.token = s.substr(6);
        }
        fclose(f);
    }
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--server") o.server = next();
        else if (a == L"--token") o.token = next();
        else if (a == L"--jobs") o.jobs = _wtoi(next().c_str());
        else if (a == L"--dxc") o.dxc = next();
        else if (a == L"--timeout") o.timeout = _wtoi(next().c_str());
        else {
            std::printf("usage: flamingo-worker [--server http://IP:PORT] [--token T] [--jobs N] [--dxc path] "
                        "[--timeout S]\n(server and token come from worker.txt next to this program if not given)\n");
            return 2;
        }
    }
    if (o.jobs <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        o.jobs = (int)si.dwNumberOfProcessors;
    }
    if (GetFileAttributesW(o.dxc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("dxc.exe not found at %ls\n", o.dxc.c_str());
        return 1;
    }
    Url u;
    if (o.server.empty() || o.token.empty()) {
        std::printf("no server or token: put worker.txt (from the Flamingo Port launcher) next to this program\n");
        return 2;
    }
    if (!crack(o.server, u)) {
        std::printf("bad server address: %ls\n", o.server.c_str());
        return 2;
    }
    g_headers = L"X-Protocol: 1\r\nX-Token: " + o.token;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    {
        // a helper whose DXC can't run would fail every shader it takes: check it before taking any
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"flamingo-worker";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\selftest";
        CreateDirectoryW(dir.c_str(), nullptr);
        Reply probe;
        probe.body = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }\n";
        probe.profile = L"ps_6_6";
        probe.opt = L"-O0";
        std::string out;
        if (!compile(o, dir, probe, out)) {
            std::printf("DXC doesn't work on this PC (%s): check that dxc.exe, dxcompiler.dll and the msvcp140 / "
                        "vcruntime140 DLLs are next to this program\n", out.c_str());
            return 1;
        }
    }
    say("flamingo-worker on %ls: %d jobs, server %ls", g_name, o.jobs, o.server.c_str());
    std::vector<std::thread> threads;
    for (int n = 0; n < o.jobs; ++n) threads.emplace_back(work, std::cref(o), std::cref(u), n);
    for (auto& t : threads) t.join();
    say("done: %d compiled, %d failed", g_ok.load(), g_failed.load());
    return 0;
}
