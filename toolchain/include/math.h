#pragma once
// hc09 shim: the math functions the recompiled code and SIMDe use, declared exactly as the UCRT declares them on x64
// with /MD (_ACRTIMP = dllimport where UCRT has it), so the code is byte-identical to a build with MSVC's headers.
#define _ACRTIMP __declspec(dllimport)
#ifdef __cplusplus
extern "C" {
#endif
double __cdecl sqrt(double);
_ACRTIMP float __cdecl sqrtf(float);
double __cdecl fabs(double);
_ACRTIMP float __cdecl fabsf(float);
_ACRTIMP double __cdecl floor(double);
_ACRTIMP float __cdecl floorf(float);
_ACRTIMP double __cdecl ceil(double);
_ACRTIMP float __cdecl ceilf(float);
_ACRTIMP double __cdecl trunc(double);
_ACRTIMP float __cdecl truncf(float);
_ACRTIMP double __cdecl round(double);
_ACRTIMP float __cdecl roundf(float);
_ACRTIMP double __cdecl nearbyint(double);
_ACRTIMP float __cdecl nearbyintf(float);
_ACRTIMP double __cdecl rint(double);
_ACRTIMP float __cdecl rintf(float);
double __cdecl fmod(double, double);
_ACRTIMP float __cdecl fmodf(float, float);
_ACRTIMP double __cdecl copysign(double, double);
_ACRTIMP float __cdecl copysignf(float, float);
_ACRTIMP double __cdecl fmin(double, double);
_ACRTIMP float __cdecl fminf(float, float);
_ACRTIMP double __cdecl fmax(double, double);
_ACRTIMP float __cdecl fmaxf(float, float);
_ACRTIMP double __cdecl fma(double, double, double);
_ACRTIMP float __cdecl fmaf(float, float, float);
_ACRTIMP double __cdecl exp2(double);
_ACRTIMP float __cdecl exp2f(float);
_ACRTIMP double __cdecl log2(double);
_ACRTIMP float __cdecl log2f(float);
double __cdecl exp(double);
_ACRTIMP float __cdecl expf(float);
double __cdecl log(double);
_ACRTIMP float __cdecl logf(float);
double __cdecl pow(double, double);
_ACRTIMP float __cdecl powf(float, float);
double __cdecl sin(double);
_ACRTIMP float __cdecl sinf(float);
double __cdecl cos(double);
_ACRTIMP float __cdecl cosf(float);
#ifdef __cplusplus
}
#endif
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
