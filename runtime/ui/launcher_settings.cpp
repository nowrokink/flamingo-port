// The launcher's settings (launcher.json), the user's copy of the game and the port's folder, and what helper PCs
// need to join (the token, the helper's folder with a worker.txt naming this PC).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <thread>
#include <vector>

#include "core/fs.h"
#include "core/system.h"
#include "prep/versions.h"
#include "ui/app.h"

namespace fp::launcher {

std::wstring lan_address() {
    ULONG size = 16384;
    std::vector<uint8_t> buf(size);
    auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, a, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, a, &size) != NO_ERROR)
            return L"127.0.0.1";
    }
    std::wstring first;
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* in = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&in->sin_addr);
            wchar_t text[32];
            swprintf_s(text, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (b[0] == 127) continue;
            if (first.empty()) first = text;
            if ((b[0] == 192 && b[1] == 168) || b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31)) return text;
        }
    }
    return first.empty() ? L"127.0.0.1" : first;
}

std::string random_hex(int bytes) {
    std::vector<uint8_t> b(static_cast<size_t>(bytes));
    BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t x : b) {
        out += digits[x >> 4];
        out += digits[x & 15];
    }
    return out;
}

// ---------------------------------------------------------------- settings, copy and folders

void App::load_settings() {
    const std::wstring app = local_appdata();
    for (const std::wstring& p : {join(app, L"launcher.json")}) {
        json::Value v;
        if (json::load_file(p, v) && v.is_object()) {
            s_ = v;
            break;
        }
    }
    game_ = w(s_["game"].str());
    out_ = s_["out"].str().empty() ? prep::default_out() : w(s_["out"].str());
    if (s_["shader_stats"].is_object()) stats_ = s_["shader_stats"];
}

void App::remember() {
    s_.set("game", narrow(game_));
    s_.set("out", narrow(out_));
    s_.set("shader_stats", stats_);
    make_dirs(local_appdata());
    json::save_file(join(local_appdata(), L"launcher.json"), s_, 1);
}

std::wstring App::work() const { return s_["work"].str().empty() ? join(out_, L"work") : w(s_["work"].str()); }

bool App::port_ready() const { return is_file(join(out_, L"shaders\\index.json")); }

void App::find_game() {
    searching_ = true;
    std::thread([this] {
        auto found = prep::find_copies();
        post([this, found] {
            searching_ = false;
            if (!found.empty()) set_game(found[0].first);
        });
    }).detach();
}

void App::set_game(const std::wstring& path) {
    game_ = path;
    version_ = prep::match_version(path);
    remember();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring App::pick_folder(const wchar_t* title, const std::wstring& start) {
    std::wstring out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(title);
    if (!start.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            d->SetFolder(item);
            item->Release();
        }
    }
    if (SUCCEEDED(d->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(d->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = p;
            CoTaskMemFree(p);
            item->Release();
        }
    }
    d->Release();
    return out;
}

void App::pick_game() {
    std::wstring d = pick_folder(L"Pasta da sua cópia (a que tem MicrosoftGame.Config)", game_);
    if (!d.empty()) set_game(full_path(d));
}

void App::pick_out() {
    std::wstring d = pick_folder(L"Pasta do port", out_);
    if (d.empty()) return;
    d = full_path(d);
    if (!game_.empty() && inside(d, game_)) {
        MessageBoxW(hwnd_, L"A pasta do port precisa ficar fora da pasta do jogo.", TITLE, MB_ICONWARNING);
        return;
    }
    out_ = d;
    show_out();
    remember();
    if (mode_ == Mode::idle || mode_ == Mode::ready) mode_ = port_ready() ? Mode::ready : Mode::idle;
}

void App::show_out() {
    if (out_measuring_) return;
    out_measuring_ = true;
    std::wstring out = out_;
    std::thread([this, out] {
        double free = free_gb(out);
        double used = is_dir(out) ? folder_gb(out) : 0.0;
        post([this, free, used] {
            out_free_ = free;
            out_used_ = used;
            out_measuring_ = false;
        });
    }).detach();
}

void App::open_folder(const std::wstring& p) {
    make_dirs(p);
    ShellExecuteW(hwnd_, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string App::helper_token() {
    if (s_["helper_token"].str().empty()) {
        s_.set("helper_token", random_hex(16));
        remember();
    }
    return s_["helper_token"].str();
}

void App::helper_folder() {
    // the helper's folder (tools\worker\build: flamingo-worker.exe, dxc), with a worker.txt for this PC
    const std::wstring folder = join(repo_root(), L"tools\\worker\\build");
    if (!is_file(join(folder, L"flamingo-worker.exe"))) {
        MessageBoxW(hwnd_, L"O ajudante ainda não foi compilado: rode scripts\\build_worker.ps1.", TITLE, MB_ICONINFORMATION);
        return;
    }
    const int64_t port = s_["helper_port"].integer(8765);
    std::string text = "server=http://" + narrow(lan_address()) + ":" + std::to_string(port) + "\ntoken=" +
                       helper_token() + "\n";
    write_file(join(folder, L"worker.txt"), text.data(), text.size());
    MessageBoxW(hwnd_,
                L"Copie esta pasta para o outro PC e abra o flamingo-worker.exe lá. Ele fica esperando e ajuda sempre que "
                L"este PC estiver preparando.\n\nO worker.txt tem o endereço deste PC e a senha dos ajudantes.",
                TITLE, MB_ICONINFORMATION);
    open_folder(folder);
}

}  // namespace fp::launcher
