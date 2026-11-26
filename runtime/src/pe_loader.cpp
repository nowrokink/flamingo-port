#include "pe_loader.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "../shims/shims.h"

namespace rt {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

namespace {

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}

template <class T>
T* at(uint8_t* base, uint32_t rva) { return reinterpret_cast<T*>(base + rva); }

bool contains_range(uint64_t offset, uint64_t length, size_t total) {
    return offset <= total && length <= total - static_cast<size_t>(offset);
}

template <class T>
T read_at(const uint8_t* base, size_t size, uint64_t offset, const char* what) {
    if (!contains_range(offset, sizeof(T), size) || offset % alignof(T) != 0)
        throw std::runtime_error(std::string("invalid ") + what + " range");
    T value;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

template <class T>
T* checked_at(uint8_t* base, size_t size, uint64_t offset, const char* what) {
    if (!contains_range(offset, sizeof(T), size) || offset % alignof(T) != 0)
        throw std::runtime_error(std::string("invalid ") + what + " range");
    return reinterpret_cast<T*>(base + offset);
}

// A bounds-checked array inside the image. PE tables need not be aligned (the Windows loader accepts
// unaligned export tables, and game_scarlett_master.exe 1.0.2.1 has one at an odd RVA), so elements
// are read with memcpy instead of through a T*.
template <class T>
struct Table {
    const uint8_t* p;
    T operator[](uint64_t i) const {
        T value;
        std::memcpy(&value, p + i * sizeof(T), sizeof(T));
        return value;
    }
};

template <class T>
Table<T> checked_table(const uint8_t* base, size_t size, uint32_t rva, uint64_t count, const std::string& what) {
    if (count > std::numeric_limits<uint64_t>::max() / sizeof(T) || !contains_range(rva, count * sizeof(T), size))
        throw std::runtime_error("invalid " + what + " table");
    return {base + rva};
}

const char* checked_string(uint8_t* base, size_t size, uint64_t rva, const char* what) {
    if (rva >= size)
        throw std::runtime_error(std::string("invalid ") + what + " string");
    auto* value = reinterpret_cast<const char*>(base + rva);
    if (!std::memchr(value, '\0', size - static_cast<size_t>(rva)))
        throw std::runtime_error(std::string("unterminated ") + what + " string");
    return value;
}

IMAGE_DATA_DIRECTORY data_directory(const IMAGE_OPTIONAL_HEADER64& header, unsigned index) {
    if (index >= header.NumberOfRvaAndSizes || index >= IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
        return {};
    return header.DataDirectory[index];
}

// The Xbox builds of the MSVC runtime import the same CRT the desktop does, so the host's own copies are used.
bool prefer_host_copy(const std::string& dll) {
    static const char* names[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll",
                                  "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
                                  "concrt140.dll", "vccorlib140.dll", "vcomp140.dll"};
    for (auto* n : names)
        if (dll == n) return true;
    return false;
}

}  // namespace

Loader::~Loader() {
    for (auto* m : order_) {
        if (m->os_mapped) FreeLibrary(reinterpret_cast<HMODULE>(m->base));
        else if (m->base) VirtualFree(m->base, 0, MEM_RELEASE);
        delete m;
    }
}

Module& Loader::load(const std::filesystem::path& image) {
    std::string key = lower(image.filename().string());
    if (auto it = by_name_.find(key); it != by_name_.end()) return *it->second;
    Module& m = map_image(image);
    bind_imports(m);
    protect_sections(m);
    return m;
}

Module& Loader::map_image(const std::filesystem::path& path) {
    auto file = read_file(path);
    if (file.size() < sizeof(IMAGE_DOS_HEADER))
        throw std::runtime_error(path.string() + ": not an MZ image");
    const auto dos = read_at<IMAGE_DOS_HEADER>(file.data(), file.size(), 0, "DOS header");
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<uint32_t>(dos.e_lfanew) % alignof(DWORD) != 0)
        throw std::runtime_error(path.string() + ": not a valid MZ image");
    const uint64_t nt_offset = static_cast<uint32_t>(dos.e_lfanew);
    const auto nt = read_at<IMAGE_NT_HEADERS64>(file.data(), file.size(), nt_offset, "NT headers");
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
        throw std::runtime_error(path.string() + ": not an x86-64 PE");
    const auto& oh = nt.OptionalHeader;
    const uint64_t section_offset = nt_offset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                    nt.FileHeader.SizeOfOptionalHeader;
    const uint64_t section_bytes = static_cast<uint64_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (!contains_range(section_offset, section_bytes, file.size()) ||
        section_offset % alignof(IMAGE_SECTION_HEADER) != 0 ||
        !contains_range(section_offset, section_bytes, oh.SizeOfHeaders) ||
        oh.SizeOfImage == 0 || oh.SizeOfHeaders > file.size() || oh.SizeOfHeaders > oh.SizeOfImage ||
        oh.ImageBase > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        (oh.AddressOfEntryPoint && oh.AddressOfEntryPoint >= oh.SizeOfImage))
        throw std::runtime_error(path.string() + ": invalid PE image or section headers");

    for (unsigned index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        const auto sec = read_at<IMAGE_SECTION_HEADER>(
            file.data(), file.size(), section_offset + static_cast<uint64_t>(index) * sizeof(IMAGE_SECTION_HEADER),
            "section header");
        const uint64_t virtual_size = sec.Misc.VirtualSize ? sec.Misc.VirtualSize : sec.SizeOfRawData;
        if ((virtual_size && sec.VirtualAddress < oh.SizeOfHeaders) ||
            !contains_range(sec.VirtualAddress, virtual_size, oh.SizeOfImage) ||
            (sec.SizeOfRawData && !contains_range(sec.PointerToRawData, sec.SizeOfRawData, file.size())))
            throw std::runtime_error(path.string() + ": invalid PE section range");
    }

    for (const unsigned index : {IMAGE_DIRECTORY_ENTRY_EXPORT, IMAGE_DIRECTORY_ENTRY_IMPORT,
                                 IMAGE_DIRECTORY_ENTRY_BASERELOC, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT}) {
        const auto dir = data_directory(oh, index);
        if ((dir.Size == 0) != (dir.VirtualAddress == 0) ||
            (dir.Size && !contains_range(dir.VirtualAddress, dir.Size, oh.SizeOfImage)))
            throw std::runtime_error(path.string() + ": invalid PE data directory");
    }

    auto* m = new Module;
    m->name = path.filename().string();
    m->path = path;
    m->preferred_base = oh.ImageBase;
    m->size = oh.SizeOfImage;
    m->entry_rva = oh.AddressOfEntryPoint;

    // Preferred: let Windows map the image. It relocates it and registers it as a loaded module, so ntdll's own
    // stack walks (RtlCaptureStackBackTrace, the loader's caller checks) can unwind through its code; they ignore
    // RtlAddFunctionTable tables, and GetModuleHandleW crashed walking through manually mapped game frames.
    // DONT_RESOLVE_DLL_REFERENCES: no imports resolved, no entry point or TLS callbacks run, no static TLS slot.
    if (HMODULE os = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES)) {
        m->base = reinterpret_cast<uint8_t*>(os);
        m->os_mapped = true;
        m->relocated = reinterpret_cast<uintptr_t>(os) != oh.ImageBase;
        DWORD old;
        if (!VirtualProtect(m->base, m->size, PAGE_EXECUTE_READWRITE, &old)) {  // copy-on-write, for binding
            const std::string name = m->name;
            FreeLibrary(os);
            delete m;
            throw std::runtime_error(name + ": can't make the mapped image writable");
        }
        order_.push_back(m);
        by_name_[lower(m->name)] = m;
        parse_exports(*m);
        return *m;
    }
    std::fprintf(stderr, "[rt] %s: Windows won't map it (error %lu), mapping it by hand\n", m->name.c_str(),
                 GetLastError());

    m->base = static_cast<uint8_t*>(
        VirtualAlloc(reinterpret_cast<void*>(oh.ImageBase), oh.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m->base) {
        const auto relocs = data_directory(oh, IMAGE_DIRECTORY_ENTRY_BASERELOC);
        if (!relocs.Size) {
            const std::string name = m->name;
            delete m;
            throw std::runtime_error(name + ": preferred base is taken and the image has no relocations");
        }
        m->base = static_cast<uint8_t*>(VirtualAlloc(nullptr, oh.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!m->base) {
            const std::string name = m->name;
            delete m;
            throw std::runtime_error(name + ": VirtualAlloc failed");
        }
    }

    std::memcpy(m->base, file.data(), oh.SizeOfHeaders);
    for (unsigned index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        const auto sec = read_at<IMAGE_SECTION_HEADER>(
            file.data(), file.size(), section_offset + static_cast<uint64_t>(index) * sizeof(IMAGE_SECTION_HEADER),
            "section header");
        const uint32_t count = std::min(sec.SizeOfRawData, sec.Misc.VirtualSize ? sec.Misc.VirtualSize : sec.SizeOfRawData);
        if (count)
            std::memcpy(m->base + sec.VirtualAddress, file.data() + sec.PointerToRawData, count);
    }

    order_.push_back(m);
    by_name_[lower(m->name)] = m;

    int64_t delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(m->base)) - static_cast<int64_t>(oh.ImageBase);
    if (delta) {
        m->relocated = true;
        const auto dir = data_directory(oh, IMAGE_DIRECTORY_ENTRY_BASERELOC);
        if (!dir.Size)
            throw std::runtime_error(m->name + ": preferred base is taken and the image has no relocations");
        uint32_t cursor = 0;
        while (cursor < dir.Size) {
            if (dir.Size - cursor < sizeof(IMAGE_BASE_RELOCATION))
                throw std::runtime_error(m->name + ": truncated relocation block");
            auto block = read_at<IMAGE_BASE_RELOCATION>(
                m->base, m->size, static_cast<uint64_t>(dir.VirtualAddress) + cursor, "relocation block");
            if (block.SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || block.SizeOfBlock > dir.Size - cursor ||
                (block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) % sizeof(uint16_t) != 0)
                throw std::runtime_error(m->name + ": invalid relocation block size");
            const uint32_t count = (block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
            for (uint32_t index = 0; index < count; ++index) {
                uint16_t entry = read_at<uint16_t>(
                    m->base, m->size,
                    static_cast<uint64_t>(dir.VirtualAddress) + cursor + sizeof(IMAGE_BASE_RELOCATION) +
                        static_cast<uint64_t>(index) * sizeof(uint16_t),
                    "relocation entry");
                uint16_t type = entry >> 12, off = entry & 0xFFF;
                uint64_t target_rva = static_cast<uint64_t>(block.VirtualAddress) + off;
                if (type == IMAGE_REL_BASED_DIR64)
                    *checked_at<uint64_t>(m->base, m->size, target_rva, "relocation target") += delta;
                else if (type != IMAGE_REL_BASED_ABSOLUTE)
                    throw std::runtime_error(m->name + ": unsupported relocation type " + std::to_string(type));
            }
            cursor += block.SizeOfBlock;
        }
    }
    parse_exports(*m);
    return *m;
}

void Loader::parse_exports(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    const auto dir = data_directory(nt->OptionalHeader, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!dir.Size) return;
    if (dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
        throw std::runtime_error(m.name + ": truncated export directory");
    auto ed = read_at<IMAGE_EXPORT_DIRECTORY>(m.base, m.size, dir.VirtualAddress, "export directory");
    if (ed.NumberOfFunctions > std::numeric_limits<uint32_t>::max() - ed.Base)
        throw std::runtime_error(m.name + ": invalid export ordinal range");
    auto funcs = checked_table<uint32_t>(m.base, m.size, ed.AddressOfFunctions, ed.NumberOfFunctions, m.name + ": export function");
    auto names = checked_table<uint32_t>(m.base, m.size, ed.AddressOfNames, ed.NumberOfNames, m.name + ": export name");
    auto ords = checked_table<uint16_t>(m.base, m.size, ed.AddressOfNameOrdinals, ed.NumberOfNames, m.name + ": export ordinal");
    for (uint32_t i = 0; i < ed.NumberOfFunctions; ++i) {
        if (funcs[i]) {
            if (funcs[i] >= m.size)
                throw std::runtime_error(m.name + ": invalid exported function RVA");
            if (funcs[i] >= dir.VirtualAddress && funcs[i] - dir.VirtualAddress < dir.Size)
                throw std::runtime_error(m.name + ": forwarded exports are not supported");
            m.exports["#" + std::to_string(ed.Base + i)] = m.base + funcs[i];
        }
    }
    for (uint32_t i = 0; i < ed.NumberOfNames; ++i) {
        if (ords[i] >= ed.NumberOfFunctions || !funcs[ords[i]])
            throw std::runtime_error(m.name + ": invalid exported name ordinal");
        const char* name = checked_string(m.base, m.size, names[i], "export name");
        if (funcs[ords[i]] >= m.size)
            throw std::runtime_error(m.name + ": invalid exported function RVA");
        m.exports[name] = m.base + funcs[ords[i]];
    }
    // Forwarded exports are rejected until forwarding is implemented.
}

void* Loader::resolve(Module& m, const std::string& dll, const std::string& name, uint16_t ordinal, bool by_ordinal) {
    std::string key = lower(dll);
    std::string sym = by_ordinal ? "#" + std::to_string(ordinal) : name;

    if (void* p = shims::find(key, sym)) {
        m.imports.push_back({dll, sym, Binding::Shim});
        return trace_host_ ? shims::make_trace_thunk(dll, sym, p) : p;
    }
    // Bundled game module (an Xbox build that ships beside the game)?
    auto bundled = game_dir_ / dll;
    if (!prefer_host_copy(key) && std::filesystem::exists(bundled)) {
        Module& dep = load(bundled);
        if (auto it = dep.exports.find(sym); it != dep.exports.end()) {
            m.imports.push_back({dll, sym, Binding::Module});
            return it->second;
        }
    }
    // Host Windows: system DLLs and API sets. LOAD_LIBRARY_SEARCH_SYSTEM32 keeps the game folder out of the search.
    if (HMODULE h = LoadLibraryExA(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        FARPROC p = GetProcAddress(h, by_ordinal ? MAKEINTRESOURCEA(ordinal) : name.c_str());
        if (p) {
            m.imports.push_back({dll, sym, Binding::Host});
            return trace_host_ ? shims::make_trace_thunk(dll, sym, reinterpret_cast<void*>(p))
                               : reinterpret_cast<void*>(p);
        }
    }
    m.imports.push_back({dll, sym, Binding::Unimplemented});
    return shims::make_unimplemented_thunk(dll, sym);
}

void Loader::bind_imports(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);

    auto bind_thunks = [&](const std::string& dll, uint32_t lookup_rva, uint32_t iat_rva) {
        if (!lookup_rva || !iat_rva)
            throw std::runtime_error(m.name + ": missing import thunk table");
        for (uint64_t index = 0; ; ++index) {
            const uint64_t offset = index * sizeof(IMAGE_THUNK_DATA64);
            auto lookup = read_at<IMAGE_THUNK_DATA64>(m.base, m.size, lookup_rva + offset, "import thunk");
            auto* iat = checked_at<IMAGE_THUNK_DATA64>(
                m.base, m.size, iat_rva + offset, "import address thunk");
            if (!lookup.u1.AddressOfData) return;
            bool by_ord = IMAGE_SNAP_BY_ORDINAL64(lookup.u1.Ordinal);
            if (by_ord && (lookup.u1.Ordinal & 0x7FFFFFFFFFFF0000ULL))
                throw std::runtime_error(m.name + ": invalid ordinal import thunk");
            uint16_t ord = by_ord ? static_cast<uint16_t>(IMAGE_ORDINAL64(lookup.u1.Ordinal)) : 0;
            std::string name;
            if (!by_ord) {
                if (lookup.u1.AddressOfData > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error(m.name + ": invalid import name RVA");
                const uint32_t name_rva = static_cast<uint32_t>(lookup.u1.AddressOfData);
                read_at<WORD>(m.base, m.size, name_rva, "import hint");
                name = checked_string(m.base, m.size, static_cast<uint64_t>(name_rva) +
                                          offsetof(IMAGE_IMPORT_BY_NAME, Name), "import name");
            }
            iat->u1.Function = reinterpret_cast<uint64_t>(resolve(m, dll, name, ord, by_ord));
        }
    };

    const auto dir = data_directory(nt->OptionalHeader, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (dir.Size) {
        if (dir.Size % sizeof(IMAGE_IMPORT_DESCRIPTOR) != 0)
            throw std::runtime_error(m.name + ": invalid import descriptor table size");
        bool terminated = false;
        const uint64_t count = dir.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
        for (uint64_t index = 0; index < count; ++index) {
            auto d = read_at<IMAGE_IMPORT_DESCRIPTOR>(
                m.base, m.size, static_cast<uint64_t>(dir.VirtualAddress) +
                                    index * sizeof(IMAGE_IMPORT_DESCRIPTOR),
                "import descriptor");
            if (!d.Name) {
                terminated = true;
                break;
            }
            const std::string dll = checked_string(m.base, m.size, d.Name, "import DLL name");
            const uint32_t lookup = d.OriginalFirstThunk ? d.OriginalFirstThunk : d.FirstThunk;
            bind_thunks(dll, lookup, d.FirstThunk);
        }
        if (!terminated) throw std::runtime_error(m.name + ": unterminated import descriptor table");
    }
    // Delay imports are bound eagerly: simpler, and the full import surface shows up in --dry-run.
    const auto ddir = data_directory(nt->OptionalHeader, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (ddir.Size) {
        if (ddir.Size % sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) != 0)
            throw std::runtime_error(m.name + ": invalid delay import descriptor table size");
        bool terminated = false;
        const uint64_t count = ddir.Size / sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
        for (uint64_t index = 0; index < count; ++index) {
            auto d = read_at<IMAGE_DELAYLOAD_DESCRIPTOR>(
                m.base, m.size, static_cast<uint64_t>(ddir.VirtualAddress) +
                                    index * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR),
                "delay import descriptor");
            if (!d.DllNameRVA) {
                terminated = true;
                break;
            }
            if (d.Attributes.AllAttributes != 1)
                throw std::runtime_error(m.name + ": unsupported delay import descriptor attributes");
            const std::string dll = checked_string(m.base, m.size, d.DllNameRVA, "delay import DLL name");
            bind_thunks(dll, d.ImportNameTableRVA, d.ImportAddressTableRVA);
        }
        if (!terminated) throw std::runtime_error(m.name + ": unterminated delay import descriptor table");
    }
}

void Loader::protect_sections(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    DWORD old;
    VirtualProtect(m.base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        bool x = sec->Characteristics & IMAGE_SCN_MEM_EXECUTE, w = sec->Characteristics & IMAGE_SCN_MEM_WRITE;
        DWORD prot = x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (w ? PAGE_READWRITE : PAGE_READONLY);
        SIZE_T size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (size) VirtualProtect(m.base + sec->VirtualAddress, size, prot, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), m.base, m.size);
}

}  // namespace rt
