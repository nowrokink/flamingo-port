// Child processes: started in a job object (stopped as a whole), with their output gathered.
#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fp {

// A child process tree that can be stopped as a whole (a job object that also ends with this process).
class Job {
public:
    Job();
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    void kill();
    HANDLE handle() const { return job_; }

private:
    HANDLE job_ = nullptr;
};

struct Spawn {
    std::wstring exe;                  // full path
    std::vector<std::wstring> args;    // quoted as needed
    std::wstring cwd;
    std::vector<std::pair<std::wstring, std::wstring>> env;  // set on top of this process's environment
    DWORD priority = BELOW_NORMAL_PRIORITY_CLASS;
    Job* job = nullptr;
    DWORD timeout_ms = INFINITE;       // run_capture: ended (and -2 returned) past this
};

std::wstring quote(const std::wstring& arg);
std::wstring command_line(const Spawn& s);

// Runs to the end with `input` on stdin; stdout and stderr gathered. -1 if it couldn't start, -2 if it ran past
// s.timeout_ms (it is ended then).
int run_capture(const Spawn& s, const std::string& input, std::string& out, std::string& err);
// Runs to the end, every line of stdout and stderr (merged, UTF-8, without the newline) given to `line`. -1 if it
// couldn't start.
int run_lines(const Spawn& s, const std::function<void(const std::string&)>& line);

}  // namespace fp
