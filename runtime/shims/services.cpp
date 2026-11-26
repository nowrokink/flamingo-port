// Small runtime areas 1.0.2.1 asks for at start (signatures from the public GDK documentation; slots from
// gdk_api.inc):
//
//   XPackage     3 XPackageGetCurrentProcessPackageIdentifier(size, buffer), 4 XPackageIsPackagedProcess(),
//                39 XPackageEnumeratePackages(kind, scope, context, callback): no other packages installed
//   XNetworking  15 XNetworkingGetConnectivityHint(hint*): offline, 16 / 17 (un)register for its changes
//   XUser        33 XUserRegisterForChangeEvent(queue, context, callback, token*), 34 XUserUnregisterForChangeEvent
//   XDisplay     4 XDisplayTryEnableHdrMode(mode, info*): SDR
//   XGame        3 XGameGetXboxTitleId(id*)
//
// The title's identity comes from its MicrosoftGame.Config (set_game_dir).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "shims.h"
#include "xgameruntime.h"

namespace {

namespace xgr = rt::xgr;

struct Identity {
    std::string name = "Flamingo.Title", version = "1.0.0.0", title_id = "00000000";
};
Identity* g_identity = new Identity;

std::string attribute(const std::string& text, const char* tag, const char* attr) {
    size_t at = text.find(std::string("<") + tag);
    if (at == std::string::npos) return {};
    size_t end = text.find('>', at);
    std::string key = std::string(attr) + "=\"";
    size_t a = text.find(key, at);
    if (a == std::string::npos || a > end) return {};
    a += key.size();
    return text.substr(a, text.find('"', a) - a);
}
std::string element(const std::string& text, const char* tag) {
    std::string open = std::string("<") + tag + ">";
    size_t a = text.find(open);
    if (a == std::string::npos) return {};
    a += open.size();
    return text.substr(a, text.find('<', a) - a);
}

constexpr GUID guid(uint32_t a, uint16_t b, uint16_t c, uint64_t d) {
    return {a, b, c, {static_cast<uint8_t>(d >> 56), static_cast<uint8_t>(d >> 48), static_cast<uint8_t>(d >> 40),
                      static_cast<uint8_t>(d >> 32), static_cast<uint8_t>(d >> 24), static_cast<uint8_t>(d >> 16),
                      static_cast<uint8_t>(d >> 8), static_cast<uint8_t>(d)}};
}

// ---- XPackage

std::string package_id() { return g_identity->name + "_" + g_identity->version + "_x64__flamingo"; }

HRESULT WINAPI package_identifier(void*, size_t size, char* buffer) {
    std::string id = package_id();
    if (!buffer || size < id.size() + 1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(buffer, id.c_str(), id.size() + 1);
    return S_OK;
}
bool WINAPI package_is_packaged(void*) { return true; }
HRESULT WINAPI package_enumerate(void*, uint32_t kind, uint32_t scope, void*, void*) {
    static std::atomic<int> logged{0};
    if (logged++ < 4)
        std::fprintf(stderr, "[rt] xpackage: EnumeratePackages(kind %u, scope %u): none\n", kind, scope);
    return S_OK;
}

// ---- XNetworking

struct ConnectivityHint {
    uint32_t level;  // XNetworkingConnectivityLevelHint: 0 unknown, 1 none, 2 local access, 3 internet access
    uint32_t cost;   // XNetworkingConnectivityCostHint: 0 unknown, 1 unrestricted
    uint32_t iana_interface_type;
    bool network_initialized;
    bool approaching_data_limit, over_data_limit, roaming;
};
static_assert(sizeof(ConnectivityHint) == 16);
HRESULT WINAPI connectivity_hint(void*, ConnectivityHint* h) {
    if (!h) return E_POINTER;
    *h = {1, 1, 0, true, false, false, false};
    return S_OK;
}

// Registrations for change events that never come offline: a token, nothing else.
std::atomic<uint64_t> g_tokens{1};
HRESULT WINAPI register_event(void*, void*, void*, void*, uint64_t* token) {
    if (!token) return E_POINTER;
    *token = g_tokens++;
    return S_OK;
}
bool WINAPI unregister_event(void*, uint64_t, bool) { return true; }

HRESULT copy_text(const char* text, size_t size, char* buffer, size_t* used);

// ---- XUser: one offline local user, signed in, never a guest, an adult with every privilege. A handle is the
// address of the one user; duplicating and closing are free. Tokens come from the online service: none offline.
// (Values as the VIctory project serves them.)

struct User {
    int unused;
};
User g_the_user;
constexpr uint64_t kXuid = 0x0009000000000001ull, kLocalId = 1;
constexpr HRESULT kUserNotFound = static_cast<HRESULT>(0x89245104);
constexpr HRESULT kNoToken = static_cast<HRESULT>(0x89245111);
const char kAddIdentity = 0, kTokenIdentity = 0, kResolveIdentity = 0;

template <typename T>
HRESULT put(T* out, T value) {
    if (!out) return E_POINTER;
    *out = value;
    return S_OK;
}
HRESULT user_found(bool found, User** out) { return found ? put(out, &g_the_user) : kUserNotFound; }

HRESULT WINAPI user_duplicate(void*, User*, User** out) { return put(out, &g_the_user); }
void WINAPI user_close(void*, User*) {}
int32_t WINAPI user_compare(void*, User* a, User* b) { return a != b; }
HRESULT WINAPI user_max(void*, uint32_t* out) { return put(out, 1u); }
HRESULT WINAPI user_add_async(void*, uint32_t options, void* block) {
    std::fprintf(stderr, "[rt] xuser: XUserAddAsync(options %#x): the offline local user\n", options);
    return xgr::finish_async(block, &kAddIdentity, S_OK);
}
HRESULT WINAPI user_add_result(void*, void* block, User** out) {
    HRESULT hr = xgr::async_result(block, &kAddIdentity);
    return FAILED(hr) ? hr : put(out, &g_the_user);
}
HRESULT WINAPI user_local_id(void*, User*, uint64_t* out) { return put(out, kLocalId); }
HRESULT WINAPI user_find_by_local_id(void*, uint64_t id, User** out) { return user_found(id == kLocalId, out); }
HRESULT WINAPI user_id(void*, User*, uint64_t* out) { return put(out, kXuid); }
HRESULT WINAPI user_find_by_id(void*, uint64_t id, User** out) { return user_found(id == kXuid, out); }
HRESULT WINAPI user_is_guest(void*, User*, bool* out) { return put(out, false); }
HRESULT WINAPI user_state(void*, User*, uint32_t* out) { return put(out, 0u); }  // signed in
// (handle, component, size, buffer, used): no suffix, so the modern suffix is empty; every other part is the name
HRESULT WINAPI user_gamertag(void*, User*, uint32_t component, size_t size, char* buffer, size_t* used) {
    return copy_text(component == 2 ? "" : "Player", size, buffer, used);
}
HRESULT WINAPI user_age_group(void*, User*, uint32_t* out) { return put(out, 3u); }  // adult
HRESULT WINAPI user_check_privilege(void*, User*, uint32_t, uint32_t, bool* has, uint32_t* reason) {
    if (reason) *reason = 0;
    return put(has, true);
}
HRESULT WINAPI user_resolve_async(void*, User*, uint32_t, uint32_t, void* block) {
    return xgr::finish_async(block, &kResolveIdentity, S_OK);
}
HRESULT WINAPI user_resolve_result(void*, void* block) { return xgr::async_result(block, &kResolveIdentity); }
HRESULT WINAPI user_token_async(void*, User*, uint32_t, const char*, const char* url, size_t, const void*, size_t,
                                const void*, void* block) {
    std::fprintf(stderr, "[rt] xuser: token for %s: none offline\n", url ? url : "?");
    return xgr::finish_async(block, &kTokenIdentity, kNoToken);
}
HRESULT WINAPI user_token_size(void*, void* block, size_t* size) {
    if (size) *size = 0;
    return xgr::async_result(block, &kTokenIdentity);
}
HRESULT WINAPI user_token_result(void*, void* block, size_t, void*, void**, size_t* used) {
    if (used) *used = 0;
    return xgr::async_result(block, &kTokenIdentity);
}

// ---- XDisplay

int32_t WINAPI display_try_hdr(void*, uint32_t, void* info) {
    if (info) std::memset(info, 0, 16);
    return 2;  // XDisplayHdrModeResult::Disabled
}

// ---- XGame

HRESULT WINAPI game_title_id(void*, uint32_t* id) {
    if (!id) return E_POINTER;
    *id = static_cast<uint32_t>(std::strtoul(g_identity->title_id.c_str(), nullptr, 16));
    return S_OK;
}

// ---- XSystem: a retail console. 1.0.2.1 checks the sandbox against the ones it knows and stops on others.

HRESULT copy_text(const char* text, size_t size, char* buffer, size_t* used) {
    size_t n = std::strlen(text) + 1;
    if (used) *used = n;
    if (!buffer || size < n) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(buffer, text, n);
    return S_OK;
}
// A fixed, neutral id: nothing about the PC goes into it.
HRESULT WINAPI system_console_id(void*, size_t size, char* buffer, size_t* used) {
    return copy_text("0000000000000000", size, buffer, used);
}
HRESULT WINAPI system_sandbox_id(void*, size_t size, char* buffer, size_t* used) {
    return copy_text("RETAIL", size, buffer, used);
}
HRESULT WINAPI system_app_device_id(void*, size_t size, char* buffer, size_t* used) {
    return copy_text("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", size, buffer, used);
}

// XSystemAnalyticsInfo XSystemGetAnalyticsInfo(): {XVersion osVersion, XVersion hostingOsVersion, char family[64],
// char form[64]}, returned through a buffer passed after the object.
struct AnalyticsInfo {
    uint64_t os_version, hosting_os_version;
    char family[64], form[64];
};
AnalyticsInfo* WINAPI system_analytics_info(void*, AnalyticsInfo* out) {
    if (!out) return out;
    *out = {};
    out->os_version = out->hosting_os_version = (10ull << 48) | (25398ull << 16);  // 10.0.25398.0
    std::strcpy(out->family, "Xbox");
    std::strcpy(out->form, rt::shims::series_s() ? "XboxScarlettLockhart" : "XboxScarlettAnaconda");
    return out;
}

// ---- XStore: a console that can't reach the store. A context is made (it grants nothing); every query completes
// with "cannot connect", as on a console without a network. No license is ever reported: the title decides what it
// does offline. (XCL takes the same line: it doesn't fake licenses.)
struct StoreContext {
    int unused;
};
StoreContext g_store;
// The documented "failed to communicate with the store network" (VIctory answers the same).
constexpr HRESULT kStoreOffline = static_cast<HRESULT>(0x89245301);
const char kStoreIdentity = 0, kLicenseIdentity = 0;
// A license handle; the user owns no add-ons, so every license handed out is this one, and not valid.
const char g_not_owned = 0;

// HRESULT XStoreAcquireLicenseForDurablesAsync(context, storeId, block) (slot 71): the durable (an add-on, e.g.
// "9PN4LLBR8RCH") isn't owned: a license that isn't valid, not an error (an error leaves 1.0.2.1 waiting on the
// loading screen; VIctory answers this way). ...Result(block, license*) (72).
HRESULT WINAPI store_durables_async(void*, StoreContext*, const char* store_id, void* block) {
    std::fprintf(stderr, "[rt] xstore: license for durable %s: not owned\n", store_id ? store_id : "?");
    const void* license = &g_not_owned;
    std::vector<uint8_t> bytes(sizeof license);
    std::memcpy(bytes.data(), &license, sizeof license);
    return xgr::finish_async_with(block, &kLicenseIdentity, S_OK, std::move(bytes));
}
HRESULT WINAPI store_license_result(void*, void* block, const void** out) {
    HRESULT hr = xgr::async_result_into(block, &kLicenseIdentity, sizeof *out, out, nullptr);
    std::fprintf(stderr, "[rt] xstore: license result %08lx\n", static_cast<unsigned long>(hr));
    return hr;
}
// bool XStoreIsLicenseValid(license) (22) / void XStoreCloseLicenseHandle(license) (23)
bool WINAPI store_license_valid(void*, const void*) {
    std::fprintf(stderr, "[rt] xstore: IsLicenseValid: no\n");
    return false;
}
void WINAPI store_close_license(void*, const void*) { std::fprintf(stderr, "[rt] xstore: license closed\n"); }

void store_log(const char* what) {
    static std::atomic<int> logged{0};
    if (logged++ < 32) std::fprintf(stderr, "[rt] xstore: %s: offline (the store can't be reached)\n", what);
}
// HRESULT XStoreCreateContext(user, context*) (slot 3) / void XStoreCloseContextHandle(context) (slot 4)
HRESULT WINAPI store_create_context(void*, void*, StoreContext** out) {
    std::fprintf(stderr, "[rt] xstore: XStoreCreateContext: a context for an offline console\n");
    return put(out, &g_store);
}
void WINAPI store_close_context(void*, StoreContext*) {}
// The ...Async calls take the context, their own arguments, and the async block last: one per argument count.
HRESULT WINAPI store_async_1(void*, StoreContext*, void* block) {
    store_log("query (context, block)");
    return xgr::finish_async(block, &kStoreIdentity, kStoreOffline);
}
HRESULT WINAPI store_async_2(void*, StoreContext*, uintptr_t, void* block) {
    store_log("query (context, 1 argument, block)");
    return xgr::finish_async(block, &kStoreIdentity, kStoreOffline);
}
HRESULT WINAPI store_async_3(void*, StoreContext*, uintptr_t, uintptr_t, void* block) {
    store_log("query (context, 2 arguments, block)");
    return xgr::finish_async(block, &kStoreIdentity, kStoreOffline);
}
HRESULT WINAPI store_async_4(void*, StoreContext*, uintptr_t, uintptr_t, uintptr_t, void* block) {
    store_log("query (context, 3 arguments, block)");
    return xgr::finish_async(block, &kStoreIdentity, kStoreOffline);
}
HRESULT WINAPI store_async_6(void*, StoreContext*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             void* block) {
    store_log("query (context, 5 arguments, block)");
    return xgr::finish_async(block, &kStoreIdentity, kStoreOffline);
}
// ...Result(block, outputs...): the block's error
HRESULT WINAPI store_result(void*, void* block) { return xgr::async_result(block, &kStoreIdentity); }

constexpr GUID kXStore = guid(0x0DD112AC, 0x7C24, 0x448C, 0xB92B3960FB5BD30Cull);
constexpr GUID kXStoreIid = guid(0xB09D803C, 0x2414, 0x4A05, 0x82C666DFDC9E9A44ull);

// ---- XSpeechSynthesizer (the accessibility area): the title's narration, silent. No voice is installed; a
// synthesizer and its streams are made, and every stream is a valid WAVE file with no samples, so the title plays
// nothing and goes on (with E_NOTIMPL from XSpeechSynthesizerCreate, 1.0.2.1 stays on the spinner after the legal
// screen; the ztrx project answers this way). Slots from gdk_api.inc, signatures from the public GDK:
//   8 EnumerateInstalledVoices(context, callback)  9 Create(handle*)  10 CloseHandle(handle)
//   11 SetDefaultVoice(handle)  12 SetCustomVoice(handle, voice id)  13 CreateStreamFromText(handle, text, stream*)
//   14 CloseStreamHandle(stream)  15 GetStreamDataSize(stream, size_t*)  16 GetStreamData(stream, size, buffer, used*)
//   21 CreateStreamFromSsmlText(handle, text, stream*)
struct Speech {
    int unused;
};
Speech g_synthesizer, g_speech_stream;
// RIFF/WAVE, PCM, mono, 16 bits, 22050 Hz, a data chunk of no bytes
const uint8_t kSilentWave[44] = {'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                                 1,   0,   1,   0,   0x22, 0x56, 0, 0, 0x44, 0xAC, 0, 0, 2, 0, 16, 0,
                                 'd', 'a', 't', 'a', 0, 0, 0, 0};
HRESULT WINAPI speech_enumerate_voices(void*, void*, void*) { return S_OK; }  // none: the callback never runs
HRESULT WINAPI speech_create(void*, Speech** out) {
    std::fprintf(stderr, "[rt] speech: a synthesizer (narration is silent)\n");
    return put(out, &g_synthesizer);
}
void WINAPI speech_close(void*, Speech*) {}
HRESULT WINAPI speech_set_voice(void*, Speech*, const char*) { return S_OK; }
HRESULT WINAPI speech_stream(void*, Speech*, const char* text, Speech** out) {
    static std::atomic<int> logged{0};
    if (logged++ < 8) std::fprintf(stderr, "[rt] speech: \"%.60s\": an empty stream\n", text ? text : "");
    return put(out, &g_speech_stream);
}
HRESULT WINAPI speech_stream_size(void*, Speech*, size_t* out) { return put(out, sizeof kSilentWave); }
HRESULT WINAPI speech_stream_data(void*, Speech*, size_t size, void* buffer, size_t* used) {
    if (!buffer || size < sizeof kSilentWave) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(buffer, kSilentWave, sizeof kSilentWave);
    if (used) *used = sizeof kSilentWave;
    return S_OK;
}
constexpr GUID kXAccessibility = guid(0x3E241977, 0x6237, 0x41E9, 0x8559102C2D9983F1ull);
constexpr GUID kXAccessibilityIid = guid(0xD722B373, 0x8C4D, 0x4692, 0x9E51D6AD9B37AA7Dull);

constexpr GUID kXSystem = guid(0xE349BD1A, 0xFC20, 0x4E40, 0xB99C4178CC6B409Full);
constexpr GUID kXSystemIid = guid(0x1861CF2E, 0xE18B, 0x4834, 0xA9F5B4A4E6EFB4CFull);
constexpr GUID kXAnalytics = guid(0xB884675D, 0xB738, 0x4A9C, 0x815D9A9A1E0C6C9Bull);

constexpr GUID kXPackage =guid(0xAF406016, 0xE850, 0x4AA8, 0xA88D2F3DCB9DAC7Eull);
constexpr GUID kXPackageIid = guid(0x0B9D556E, 0xAB70, 0x43F4, 0xBAA09999AC4CB040ull);
constexpr GUID kXNetworking = guid(0x37E56907, 0x2F10, 0x41E8, 0xB72F36EDB185331Aull);
constexpr GUID kXNetworkingIid = guid(0xBF2346B2, 0x39AF, 0x4658, 0xB5EA44713C7E83B3ull);
constexpr GUID kXUser = guid(0x01ACD177, 0x91F9, 0x4763, 0xA38ECCBB55CE32E0ull);
constexpr GUID kXDisplay = guid(0x03F0FE74, 0xFDD9, 0x4E5C, 0xB630F9339C47ACC5ull);
constexpr GUID kXDisplayIid = guid(0x35F07670, 0x706E, 0x4BFB, 0x9476090798C5EBF3ull);
constexpr GUID kXGame = guid(0x973A344E, 0x24BF, 0x4D0F, 0x845756C534892B29ull);
constexpr GUID kXGameIid = guid(0x2549F142, 0x6419, 0x4A06, 0x97B5931AAB7C2F34ull);

void* fn(auto* f) { return reinterpret_cast<void*>(f); }

xgr::AreaRegistration g_package(kXPackage, kXPackageIid,
                                xgr::make_area(kXPackage, kXPackageIid,
                                               {{3, fn(&package_identifier)}, {4, fn(&package_is_packaged)},
                                                {39, fn(&package_enumerate)}}));
xgr::AreaRegistration g_networking(kXNetworking, kXNetworkingIid,
                                   xgr::make_area(kXNetworking, kXNetworkingIid,
                                                  {{15, fn(&connectivity_hint)}, {16, fn(&register_event)},
                                                   {17, fn(&unregister_event)}}));
void* user_area(const GUID& iid) {
    return xgr::make_area(kXUser, iid,
                          {{3, fn(&user_duplicate)},       {4, fn(&user_close)},         {5, fn(&user_compare)},
                           {6, fn(&user_max)},             {7, fn(&user_add_async)},     {8, fn(&user_add_result)},
                           {9, fn(&user_local_id)},        {10, fn(&user_find_by_local_id)}, {11, fn(&user_id)},
                           {12, fn(&user_find_by_id)},     {13, fn(&user_is_guest)},     {14, fn(&user_state)},
                           {15, fn(&user_gamertag)},       {19, fn(&user_age_group)},    {20, fn(&user_check_privilege)},
                           {21, fn(&user_resolve_async)},  {22, fn(&user_resolve_result)}, {23, fn(&user_token_async)},
                           {24, fn(&user_token_size)},     {25, fn(&user_token_result)}, {33, fn(&register_event)},
                           {34, fn(&unregister_event)}});
}
constexpr GUID kXUserMain = guid(0xEB9BF948, 0x18DC, 0x4D82, 0xBBCC40E0A809C4C0ull);
constexpr GUID kXUserGamertag = guid(0xCEF4FAC0, 0x7676, 0x4A94, 0xA1194C43F9EB5B74ull);
// 1.0.2.1 asks with the provider's own id; other titles with the main interface's.
xgr::AreaRegistration g_user(kXUser, kXUser, user_area(kXUser));
xgr::AreaRegistration g_user_main(kXUser, kXUserMain, user_area(kXUserMain));
xgr::AreaRegistration g_user_gamertag(kXUser, kXUserGamertag,
                                      xgr::make_area(kXUser, kXUserGamertag, {{3, fn(&user_gamertag)}}));
xgr::AreaRegistration g_display(kXDisplay, kXDisplayIid,
                                xgr::make_area(kXDisplay, kXDisplayIid, {{4, fn(&display_try_hdr)}}));
xgr::AreaRegistration g_system(kXSystem, kXSystemIid,
                               xgr::make_area(kXSystem, kXSystemIid,
                                              {{3, fn(&system_console_id)}, {4, fn(&system_sandbox_id)},
                                               {5, fn(&system_app_device_id)}}));
xgr::AreaRegistration g_analytics(kXAnalytics, kXAnalytics,
                                  xgr::make_area(kXAnalytics, kXAnalytics, {{3, fn(&system_analytics_info)}}));
xgr::AreaRegistration g_game(kXGame, kXGameIid, xgr::make_area(kXGame, kXGameIid, {{3, fn(&game_title_id)}}));
xgr::AreaRegistration g_speech(
    kXAccessibility, kXAccessibilityIid,
    xgr::make_area(kXAccessibility, kXAccessibilityIid,
                   {{8, fn(&speech_enumerate_voices)}, {9, fn(&speech_create)},     {10, fn(&speech_close)},
                    {11, fn(&speech_set_voice)},       {12, fn(&speech_set_voice)}, {13, fn(&speech_stream)},
                    {14, fn(&speech_close)},           {15, fn(&speech_stream_size)}, {16, fn(&speech_stream_data)},
                    {21, fn(&speech_stream)}}));
// XStore slots (gdk_api.inc) by argument count. Async with (context, block): 11 ProductForCurrentGame, 28 GameLicense,
// 30 AddOnLicenses, 55 GameAndDlcPackageUpdates; (context, x, block): 20 AcquireLicenseForPackage,
// 24/26 CanAcquireLicenseFor{StoreId,Package}, 33 ConsumableBalanceRemaining; (context, x, y, block):
// 5 AssociatedProducts, 9 EntitledProducts, 13 ProductForPackage, 37 GetUserCollectionsId; (context, 5 args, block):
// 7 Products. The matching ...Result / ...ResultCount / ...ResultSize slots give the block's error.
xgr::AreaRegistration g_store_area(
    kXStore, kXStoreIid,
    xgr::make_area(kXStore, kXStoreIid,
                   {{3, fn(&store_create_context)}, {4, fn(&store_close_context)},
                    {11, fn(&store_async_1)},  {28, fn(&store_async_1)},  {30, fn(&store_async_1)},
                    {55, fn(&store_async_1)},  {20, fn(&store_async_2)},  {24, fn(&store_async_2)},
                    {26, fn(&store_async_2)},  {33, fn(&store_async_2)},  {5, fn(&store_async_3)},
                    {9, fn(&store_async_3)},   {13, fn(&store_async_3)},  {37, fn(&store_async_3)},
                    {7, fn(&store_async_6)},   {6, fn(&store_result)},    {8, fn(&store_result)},
                    {10, fn(&store_result)},   {12, fn(&store_result)},   {14, fn(&store_result)},
                    {21, fn(&store_result)},   {25, fn(&store_result)},   {27, fn(&store_result)},
                    {29, fn(&store_result)},   {31, fn(&store_result)},   {32, fn(&store_result)},
                    {34, fn(&store_result)},   {38, fn(&store_result)},   {39, fn(&store_result)},
                    {56, fn(&store_result)},   {57, fn(&store_result)},   {22, fn(&store_license_valid)},
                    {23, fn(&store_close_license)}, {71, fn(&store_durables_async)},
                    {72, fn(&store_license_result)}}));
// The other XStore interfaces 1.0.2.1 asks for: 71 AcquireLicenseForDurablesAsync(context, storeId, block) / 72 its
// result (licenses of the title's durables, e.g. "9PN4LLBR8RCH"); 77 QueryAssociatedProductsForStoreIdAsync(context,
// storeId, kinds, max, block) / 78; 79 QueryPackageUpdatesAsync(context, ids, count, block) / 80 count / 81 result;
// 70 IsAvailabilityPurchasable(availability): false.
bool WINAPI store_purchasable(void*, uintptr_t) { return false; }
constexpr GUID kXStoreDurables = guid(0x2D42FEA5, 0xE71D, 0x4B76, 0x97CDC50AFBB3AE5Dull);
constexpr GUID kXStoreAssociated = guid(0x5C48DEDF, 0x0B67, 0x4492, 0xA4B56829B8E796E1ull);
constexpr GUID kXStoreAvailability = guid(0x60B09F4E, 0x1B85, 0x45B1, 0x826C169118E230E1ull);
xgr::AreaRegistration g_store_durables(
    kXStore, kXStoreDurables,
    xgr::make_area(kXStore, kXStoreDurables,
                   {{3, fn(&store_create_context)}, {4, fn(&store_close_context)}, {22, fn(&store_license_valid)},
                    {23, fn(&store_close_license)}, {71, fn(&store_durables_async)},
                    {72, fn(&store_license_result)}}));
xgr::AreaRegistration g_store_associated(
    kXStore, kXStoreAssociated,
    xgr::make_area(kXStore, kXStoreAssociated,
                   {{77, fn(&store_async_4)}, {78, fn(&store_result)}, {79, fn(&store_async_3)},
                    {80, fn(&store_result)}, {81, fn(&store_result)}}));
xgr::AreaRegistration g_store_availability(
    kXStore, kXStoreAvailability, xgr::make_area(kXStore, kXStoreAvailability, {{70, fn(&store_purchasable)}}));

}  // namespace

void rt::shims::set_game_identity(const std::wstring& config_path) {
    std::ifstream f(config_path);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (auto v = attribute(text, "Identity", "Name"); !v.empty()) g_identity->name = v;
    if (auto v = attribute(text, "Identity", "Version"); !v.empty()) g_identity->version = v;
    if (auto v = element(text, "TitleId"); !v.empty()) g_identity->title_id = v;
    std::fprintf(stderr, "[rt] title %s, version %s, title id %s\n", g_identity->name.c_str(),
                 g_identity->version.c_str(), g_identity->title_id.c_str());
}
