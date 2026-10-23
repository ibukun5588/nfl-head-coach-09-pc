// hc09-recomp: system-information and Rtl helper imports that used to be generated
// stubs (return 0, write nothing). Found by the M8 stub audit (HC09_TRACE_STUBS=1):
// each one is called by the game, and a stub that reports success without writing
// its outputs looks like a real answer to the game (the front-end music stayed off
// for months because of one such stub, XMsgInProcessCall). Values follow a retail
// console as Xenia models it; behaviour is from the public API documentation.
#include "runtime.h"
#include <cstdio>
#include <ctime>

namespace {
constexpr uint32_t kStatusSuccess = 0;
constexpr uint32_t kStatusInvalidParameter1 = 0xC00000EF, kStatusInvalidParameter2 = 0xC00000F0,
                   kStatusInvalidParameter3 = 0xC00000F1, kStatusBufferTooSmall = 0xC0000023;
constexpr uint32_t kErrorNotFound = 0x490;   // ERROR_NOT_FOUND

// Days since 1970-01-01 <-> proleptic Gregorian date (Hinnant's civil algorithms).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = int64_t(yoe) + era * 400 + (m <= 2);
}
constexpr int64_t kDays1601To1970 = 134774;
constexpr uint64_t kTicksPerDay = 864000000000ull;   // 100 ns ticks

// Host time-zone bias in minutes, Windows convention (UTC = local + bias), DST included.
int32_t HostTimeZoneBias()
{
    time_t now = time(nullptr);
    struct tm utc{}, loc{};
#ifdef _WIN32
    gmtime_s(&utc, &now); localtime_s(&loc, &now);
#else
    gmtime_r(&now, &utc); localtime_r(&now, &loc);
#endif
    loc.tm_isdst = 0; utc.tm_isdst = 0;
    return int32_t((mktime(&utc) - mktime(&loc)) / 60);
}
}  // namespace

// DWORD KeGetCurrentProcessType(void): 1 = title (user) process. The D3D library picks
// its hardware-thread setup from it (sub_8233F7E8); 0 (idle) took the system path.
PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    ImportLog("KeGetCurrentProcessType");
    ctx.r3.u64 = 1;
}

// DWORD XGetGameRegion(void): all regions.  DWORD XGetLanguage(void): 1 = English.
PPC_FUNC(__imp__XGetGameRegion) { ImportLog("XGetGameRegion"); ctx.r3.u64 = 0xFFFF; }
PPC_FUNC(__imp__XGetLanguage)   { ImportLog("XGetLanguage");   ctx.r3.u64 = 1; }

// NTSTATUS ExGetXConfigSetting(WORD category r3, WORD setting r4, PVOID buffer r5,
//                              WORD size r6, PWORD required r7)
// The system library reads the time zone (category 3, settings 1-7) at boot to turn
// the UTC system time into local time: report the host's current offset, no DST rules.
PPC_FUNC(__imp__ExGetXConfigSetting)
{
    ImportLog("ExGetXConfigSetting");
    const uint32_t category = ctx.r3.u32 & 0xFFFF, setting = ctx.r4.u32 & 0xFFFF;
    const uint32_t buf = ctx.r5.u32, size = ctx.r6.u32 & 0xFFFF, required = ctx.r7.u32;
    uint8_t value[4] = {};
    uint32_t len = 4;
    auto be32 = [&](uint32_t v) { value[0] = v >> 24; value[1] = v >> 16; value[2] = v >> 8; value[3] = uint8_t(v); };
    uint32_t status = kStatusSuccess;
    if (category == 2) {                       // secured
        if (setting == 2) be32(0x00001000);    // AV region: USA/Canada
        else status = kStatusInvalidParameter2;
    } else if (category == 3) {                // user
        switch (setting) {
        case 1: be32(uint32_t(HostTimeZoneBias())); break;   // time zone bias
        case 2: case 3: case 4: case 5: case 6: case 7: break; // names, DST dates/biases: none
        case 9: be32(1); break;                // language: English
        case 0xA: be32(0x00040000); break;     // video flags
        case 0xC: break;                       // retail flags
        case 0xE: value[0] = 103; len = 1; break;   // country: United States
        default: status = kStatusInvalidParameter2;
        }
    } else {
        status = kStatusInvalidParameter1;
    }
    if (status == kStatusSuccess) {
        if (buf) {
            if (size < len) status = kStatusBufferTooSmall;
            else for (uint32_t i = 0; i < len; ++i) GStore8(buf + i, value[i]);
        } else if (size) {
            status = kStatusInvalidParameter3;
        }
    }
    if (status != kStatusSuccess) {
        static int logged = 0;
        if (logged++ < 8)
            fprintf(stderr, "[xconfig] ExGetXConfigSetting(%u, 0x%X) -> 0x%08X\n", category, setting, status);
        len = 0;
    }
    if (required) GStore16(required, uint16_t(len));
    ctx.r3.u64 = status;
}

// TIME_FIELDS { WORD Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday }
// VOID RtlTimeToTimeFields(PLARGE_INTEGER time r3, PTIME_FIELDS fields r4)
PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    ImportLog("RtlTimeToTimeFields");
    const uint32_t tp = ctx.r3.u32, fp = ctx.r4.u32;
    if (!tp || !fp) return;
    const uint64_t t = GLoad64(tp);
    const int64_t days = int64_t(t / kTicksPerDay) - kDays1601To1970;
    uint64_t rem = t % kTicksPerDay;
    int64_t y; unsigned m, d;
    CivilFromDays(days, y, m, d);
    const unsigned weekday = unsigned(((days % 7) + 7 + 4) % 7);   // 1970-01-01 was a Thursday
    const uint64_t ms = rem / 10000;
    GStore16(fp + 0, uint16_t(y));
    GStore16(fp + 2, uint16_t(m));
    GStore16(fp + 4, uint16_t(d));
    GStore16(fp + 6, uint16_t(ms / 3600000));
    GStore16(fp + 8, uint16_t(ms / 60000 % 60));
    GStore16(fp + 10, uint16_t(ms / 1000 % 60));
    GStore16(fp + 12, uint16_t(ms % 1000));
    GStore16(fp + 14, uint16_t(weekday));
}

// BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS fields r3, PLARGE_INTEGER time r4)
PPC_FUNC(__imp__RtlTimeFieldsToTime)
{
    ImportLog("RtlTimeFieldsToTime");
    const uint32_t fp = ctx.r3.u32, tp = ctx.r4.u32;
    ctx.r3.u64 = 0;
    if (!fp || !tp) return;
    const unsigned y = GLoad16(fp), m = GLoad16(fp + 2), d = GLoad16(fp + 4), h = GLoad16(fp + 6),
                   mi = GLoad16(fp + 8), s = GLoad16(fp + 10), ms = GLoad16(fp + 12);
    static const unsigned kMonthDays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 1601 || m < 1 || m > 12 || d < 1 || d > kMonthDays[m - 1] || h > 23 || mi > 59 || s > 59 || ms > 999)
        return;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (m == 2 && d == 29 && !leap) return;
    const int64_t days = DaysFromCivil(y, m, d) + kDays1601To1970;
    const uint64_t t = uint64_t(days) * kTicksPerDay + ((uint64_t(h) * 60 + mi) * 60 + s) * 10000000ull + uint64_t(ms) * 10000;
    GStore64(tp, t);
    ctx.r3.u64 = 1;
}

// VOID RtlFillMemoryUlong(PVOID dst r3, SIZE_T length r4, ULONG pattern r5); length % 4 == 0.
// Used by the D3D library (sub_82335738) to initialise an 800-byte table to 0x80000000.
PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    ImportLog("RtlFillMemoryUlong");
    const uint32_t dst = ctx.r3.u32, n = ctx.r4.u32 / 4, pattern = ctx.r5.u32;
    for (uint32_t i = 0; i < n; ++i) GStore32(dst + 4 * i, pattern);
}

// SIZE_T RtlCompareMemoryUlong(PVOID src r3, SIZE_T length r4, ULONG pattern r5):
// bytes that match the pattern before the first mismatch.
PPC_FUNC(__imp__RtlCompareMemoryUlong)
{
    ImportLog("RtlCompareMemoryUlong");
    const uint32_t src = ctx.r3.u32, n = ctx.r4.u32 / 4, pattern = ctx.r5.u32;
    uint32_t i = 0;
    while (i < n && GLoad32(src + 4 * i) == pattern) ++i;
    ctx.r3.u64 = uint64_t(i) * 4;
}

// ULONG RtlNtStatusToDosError(NTSTATUS r3): the system library's SetLastError path, so
// GetLastError() reports e.g. ERROR_FILE_NOT_FOUND / ERROR_NO_MORE_FILES instead of 0.
PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    ImportLog("RtlNtStatusToDosError");
    const uint32_t s = ctx.r3.u32;
    uint32_t e;
    switch (s) {
    case 0x00000000: e = 0; break;
    case 0x00000103: e = 997; break;    // STATUS_PENDING -> ERROR_IO_PENDING
    case 0x80000005: e = 234; break;    // BUFFER_OVERFLOW -> ERROR_MORE_DATA
    case 0x80000006: e = 18; break;     // NO_MORE_FILES
    case 0x8000001A: e = 259; break;    // NO_MORE_ENTRIES -> ERROR_NO_MORE_ITEMS
    case 0xC0000001: e = 31; break;     // UNSUCCESSFUL -> ERROR_GEN_FAILURE
    case 0xC0000008: e = 6; break;      // INVALID_HANDLE
    case 0xC000000D: e = 87; break;     // INVALID_PARAMETER
    case 0xC000000E: e = 2; break;      // NO_SUCH_DEVICE -> FILE_NOT_FOUND
    case 0xC000000F: e = 2; break;      // NO_SUCH_FILE -> FILE_NOT_FOUND
    case 0xC0000010: e = 1; break;      // INVALID_DEVICE_REQUEST -> INVALID_FUNCTION
    case 0xC0000011: e = 38; break;     // END_OF_FILE -> HANDLE_EOF
    case 0xC0000017: e = 8; break;      // NO_MEMORY -> NOT_ENOUGH_MEMORY
    case 0xC0000022: e = 5; break;      // ACCESS_DENIED
    case 0xC0000023: e = 122; break;    // BUFFER_TOO_SMALL -> INSUFFICIENT_BUFFER
    case 0xC0000033: e = 123; break;    // OBJECT_NAME_INVALID -> INVALID_NAME
    case 0xC0000034: e = 2; break;      // OBJECT_NAME_NOT_FOUND -> FILE_NOT_FOUND
    case 0xC0000035: e = 183; break;    // OBJECT_NAME_COLLISION -> ALREADY_EXISTS
    case 0xC000003A: e = 3; break;      // OBJECT_PATH_NOT_FOUND -> PATH_NOT_FOUND
    case 0xC0000043: e = 32; break;     // SHARING_VIOLATION
    case 0xC000007F: e = 112; break;    // DISK_FULL
    case 0xC000009A: e = 1450; break;   // INSUFFICIENT_RESOURCES -> NO_SYSTEM_RESOURCES
    case 0xC00000BA: e = 5; break;      // FILE_IS_A_DIRECTORY -> ACCESS_DENIED
    case 0xC00000BB: e = 50; break;     // NOT_SUPPORTED
    case 0xC0000101: e = 145; break;    // DIRECTORY_NOT_EMPTY -> DIR_NOT_EMPTY
    case 0xC0000103: e = 267; break;    // NOT_A_DIRECTORY -> DIRECTORY
    case 0xC0000120: e = 995; break;    // CANCELLED -> OPERATION_ABORTED
    default:
        if (s & 0x20000000) e = s;                          // customer code: as is
        else if ((s >> 16) == 0x8007 || (s >> 16) == 0xC007 || (s >> 16) == 0xC001) e = s & 0xFFFF;  // wrapped Win32
        else {
            e = 317;                                         // ERROR_MR_MID_NOT_FOUND
            static int logged = 0;
            if (logged++ < 8) fprintf(stderr, "[rtl] RtlNtStatusToDosError(0x%08X): no mapping -> 317\n", s);
        }
    }
    ctx.r3.u64 = e;
}

// DWORD XamLoaderGetLaunchDataSize(PDWORD size r3) / XamLoaderGetLaunchData(PVOID r3, DWORD r4):
// the title was started from the dashboard, so there is no launch data.
PPC_FUNC(__imp__XamLoaderGetLaunchDataSize)
{
    ImportLog("XamLoaderGetLaunchDataSize");
    if (ctx.r3.u32) GStore32(ctx.r3.u32, 0);
    ctx.r3.u64 = ctx.r3.u32 ? kErrorNotFound : 87;   // ERROR_INVALID_PARAMETER without a pointer
}
PPC_FUNC(__imp__XamLoaderGetLaunchData)
{
    ImportLog("XamLoaderGetLaunchData");
    ctx.r3.u64 = kErrorNotFound;
}
