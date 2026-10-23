#pragma once
// hc09 shim (the player's build of the generated code needs no MSVC/Windows SDK headers): the C library
// declarations the recompiled code and SIMDe use, with the MSVC x64 ABI the prebuilt runtime links against.
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) void* __cdecl malloc(size_t);
__declspec(dllimport) void* __cdecl calloc(size_t, size_t);
__declspec(dllimport) void* __cdecl realloc(void*, size_t);
__declspec(dllimport) void __cdecl free(void*);
__declspec(dllimport) void* __cdecl _aligned_malloc(size_t, size_t);
__declspec(dllimport) void __cdecl _aligned_free(void*);
__declspec(dllimport) __declspec(noreturn) void __cdecl abort(void);
__declspec(dllimport) __declspec(noreturn) void __cdecl exit(int);
int __cdecl abs(int);
long __cdecl labs(long);
long long __cdecl llabs(long long);
#ifdef __cplusplus
}
#endif
