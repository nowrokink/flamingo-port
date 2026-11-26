// Runtime API areas served through xgameruntime.dll's QueryApiImpl (see xgameruntime.cpp).
#pragma once
#include <windows.h>

#include <initializer_list>
#include <utility>
#include <vector>

namespace rt::xgr {

struct Area {
    GUID provider;
    GUID iid;
    void* object;  // a COM-style object: method table first
};

// Registers an area at startup: `static AreaRegistration reg(PROVIDER, IID, &object);`
struct AreaRegistration {
    AreaRegistration(const GUID& provider, const GUID& iid, void* object);
};

void* find_area(const GUID& provider, const GUID& iid);

// An area object whose listed slots are the given functions (each called as (object, the function's arguments...));
// every other slot logs the GDK function it stands for and returns E_NOTIMPL.
void* make_area(const GUID& provider, const GUID& iid, std::initializer_list<std::pair<int, void*>> methods);

// For areas whose async calls have an answer at once (xtaskqueue.cpp): starts the title's async block and
// completes it with `hr` (its completion routine runs on its queue as usual), and reads that result back
// (XAsyncGetResult with no buffer). `identity` tells one call's blocks from another's.
HRESULT finish_async(void* block, const void* identity, HRESULT hr);
HRESULT async_result(void* block, const void* identity);
// As finish_async, with `result` as the call's result bytes (XAsyncGetResultSize / XAsyncGetResult copy them out);
// the 8-byte fields at `rebase` hold offsets into the result and become pointers into the title's buffer.
HRESULT finish_async_with(void* block, const void* identity, HRESULT hr, std::vector<uint8_t> result,
                          std::vector<size_t> rebase = {});
// The result of such a call, into the title's buffer.
HRESULT async_result_into(void* block, const void* identity, size_t size, void* buffer, size_t* used);

}  // namespace rt::xgr
