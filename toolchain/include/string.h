#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* __cdecl memcpy(void*, const void*, size_t);
void* __cdecl memmove(void*, const void*, size_t);
void* __cdecl memset(void*, int, size_t);
int __cdecl memcmp(const void*, const void*, size_t);
size_t __cdecl strlen(const char*);
int __cdecl strcmp(const char*, const char*);
#ifdef __cplusplus
}
#endif
