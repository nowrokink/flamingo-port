// Where things are on this PC: the environment, this exe, the user's folders, the repository and the SDK tools.
#pragma once
#include <string>

namespace fp {

std::wstring env(const wchar_t* name);
std::wstring self_path();       // this exe
std::wstring local_appdata();   // %LOCALAPPDATA%\Flamingo
std::wstring documents();       // the user's Documents

// The repository this exe belongs to: the folder above it that holds game\ (the version files) and runtime.
std::wstring repo_root();

// PATH, with the Vulkan SDK's Bin folder added (the installer's change may not have reached this session).
std::wstring tool_path();
// The full path of a program found on `path` (PATH by default), or empty.
std::wstring which(const std::wstring& exe, const std::wstring& path = L"");

}  // namespace fp
