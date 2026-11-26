#include "process.h"

#include <algorithm>
#include <thread>

namespace fp {

Job::Job() {
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
}

Job::~Job() {
    if (job_) CloseHandle(job_);
}

void Job::kill() {
    if (job_) TerminateJobObject(job_, 1);
}

std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes + 1, L'\\');  // the backslashes before it doubled, and the quote escaped
            slashes = 0;
        } else {
            slashes = 0;
        }
        out += c;
    }
    out.append(slashes, L'\\');  // backslashes before the closing quote are doubled
    out += L'"';
    return out;
}

std::wstring command_line(const Spawn& s) {
    std::wstring cmd = quote(s.exe);
    for (const auto& a : s.args) cmd += L" " + quote(a);
    return cmd;
}

namespace {

// This process's environment with `extra` set on top, as a CreateProcess block.
std::wstring environment_block(const std::vector<std::pair<std::wstring, std::wstring>>& extra) {
    std::vector<std::wstring> vars;
    if (wchar_t* e = GetEnvironmentStringsW()) {
        for (const wchar_t* p = e; *p; p += wcslen(p) + 1) vars.emplace_back(p);
        FreeEnvironmentStringsW(e);
    }
    for (const auto& [k, v] : extra) {
        std::wstring prefix = k + L"=";
        vars.erase(std::remove_if(vars.begin(), vars.end(),
                                  [&](const std::wstring& x) {
                                      return x.size() >= prefix.size() &&
                                             CompareStringOrdinal(x.c_str(), static_cast<int>(prefix.size()),
                                                                  prefix.c_str(), static_cast<int>(prefix.size()),
                                                                  TRUE) == CSTR_EQUAL;
                                  }),
                   vars.end());
        vars.push_back(prefix + v);
    }
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& v : vars) {
        block += v;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

bool start(const Spawn& s, HANDLE in, HANDLE out, HANDLE err, PROCESS_INFORMATION& pi) {
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
    std::wstring cmd = command_line(s);
    std::wstring block = environment_block(s.env);
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED | s.priority;
    if (!CreateProcessW(s.exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, block.data(),
                        s.cwd.empty() ? nullptr : s.cwd.c_str(), &si, &pi))
        return false;
    if (s.job) AssignProcessToJobObject(s.job->handle(), pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return true;
}

HANDLE null_input() {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    return CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
}

void read_all(HANDLE h, std::string& out) {
    char buf[16384];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
}

}  // namespace

int run_capture(const Spawn& s, const std::string& input, std::string& out, std::string& err) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r, in_w, out_r, out_w, err_r, err_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) return -1;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) return -1;
    if (!CreatePipe(&err_r, &err_w, &sa, 0)) return -1;
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    PROCESS_INFORMATION pi{};
    bool ok = start(s, in_r, out_w, err_w, pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(err_r);
        return -1;
    }
    // stdin written and stderr read on threads of their own: either pipe filling up would stall the other
    std::thread writer([&] {
        size_t done = 0;
        while (done < input.size()) {
            DWORD put = 0;
            if (!WriteFile(in_w, input.data() + done, static_cast<DWORD>(std::min<size_t>(input.size() - done, 65536)),
                           &put, nullptr))
                break;
            done += put;
        }
        CloseHandle(in_w);
    });
    std::thread errors([&] { read_all(err_r, err); });
    // past the time limit the process is ended, which closes its pipes and so ends the reads
    bool late = false;
    std::thread watchdog([&] {
        if (s.timeout_ms != INFINITE && WaitForSingleObject(pi.hProcess, s.timeout_ms) == WAIT_TIMEOUT) {
            late = true;
            TerminateProcess(pi.hProcess, 1);
        }
    });
    read_all(out_r, out);
    writer.join();
    errors.join();
    CloseHandle(out_r);
    CloseHandle(err_r);
    WaitForSingleObject(pi.hProcess, INFINITE);
    watchdog.join();
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return late ? -2 : static_cast<int>(code);
}

int run_lines(const Spawn& s, const std::function<void(const std::string&)>& line) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_r, out_w;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) return -1;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE in = null_input();
    PROCESS_INFORMATION pi{};
    bool ok = start(s, in, out_w, out_w, pi);
    CloseHandle(out_w);
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (!ok) {
        CloseHandle(out_r);
        return -1;
    }
    std::string pending;
    char buf[16384];
    DWORD got = 0;
    while (ReadFile(out_r, buf, sizeof buf, &got, nullptr) && got) {
        pending.append(buf, got);
        size_t at = 0, nl;
        while ((nl = pending.find('\n', at)) != std::string::npos) {
            std::string l = pending.substr(at, nl - at);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            line(l);
            at = nl + 1;
        }
        pending.erase(0, at);
    }
    if (!pending.empty()) line(pending);
    CloseHandle(out_r);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace fp
