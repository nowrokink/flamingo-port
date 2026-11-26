# fp_tools_id.h: the translator's identity, a hash of its sources (the C++ form of dxil_cache.py's TOOLS_ID, which
# hashed rdna2spirv.py and dxil_cache.py). A failure kept by another identity is tried again.
#   cmake -DOUT=<header> -DFILES=<a;b;...> -P tools_id.cmake
set(all "")
list(SORT FILES)
foreach(f ${FILES})
  file(SHA1 "${f}" h)
  string(APPEND all "${h}")
endforeach()
string(SHA1 id "${all}")
string(SUBSTRING "${id}" 0 12 id)
set(text "#pragma once\n#define FP_TOOLS_ID \"${id}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
