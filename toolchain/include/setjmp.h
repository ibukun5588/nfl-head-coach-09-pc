#pragma once
// MSVC x64 layout (16 x 16 bytes)
typedef struct __declspec(align(16)) _SETJMP_FLOAT128 { unsigned long long Part[2]; } SETJMP_FLOAT128;
typedef SETJMP_FLOAT128 _JBTYPE;
typedef _JBTYPE jmp_buf[16];
#ifdef __cplusplus
extern "C" {
#endif
int __cdecl _setjmp(jmp_buf);
__declspec(noreturn) void __cdecl longjmp(jmp_buf, int);
#ifdef __cplusplus
}
#endif
#define setjmp _setjmp
