// The crash logger: see crash_report.h.
#include "crash_report.h"
#include "build_id.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

namespace {

std::atomic<bool> g_written{ false };

// "hc09rt.dll+0x1234" for a code address.
std::string ModuleOffset(const void* pc)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(pc), &m))
        return "(no module)";
    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    char buf[MAX_PATH + 32];
    snprintf(buf, sizeof buf, "%s+0x%llX", name, (unsigned long long)(uintptr_t(pc) - uintptr_t(m)));
    return buf;
}

void WriteDump(const std::string& path, _EXCEPTION_POINTERS* ep)
{
    using WriteFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    auto write = dbghelp ? reinterpret_cast<WriteFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!write) return;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
    // MiniDumpNormal: stacks, thread contexts, module list; no heap and no guest memory.
    write(GetCurrentProcess(), GetCurrentProcessId(), f, MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithUnloadedModules),
          ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* ep)
{
    char what[96];
    snprintf(what, sizeof what, "unhandled exception 0x%08lX", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0ul);
    CrashReportWrite(what, nullptr, ep);
    return EXCEPTION_CONTINUE_SEARCH;   // Windows ends the process as it would have
}

}  // namespace

void CrashReportWrite(const char* what, const char* details, _EXCEPTION_POINTERS* ep)
{
    if (g_written.exchange(true)) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[64];
    snprintf(name, sizeof name, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    CreateDirectoryA("crash-reports", nullptr);
    std::string dir = std::string("crash-reports\\") + name;
    if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;

    if (FILE* f = fopen((dir + "\\crash.txt").c_str(), "w"))
    {
        fprintf(f, "NFL Head Coach 09 for PC: crash report\n");
        fprintf(f, "build:        %s\n", HC09_BUILD_ID);
        fprintf(f, "game version: %s\n", HC09_GAME_V12 ? "1.2" : "1.0");
        fprintf(f, "time:         %04u-%02u-%02u %02u:%02u:%02u\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fprintf(f, "error:        %s\n", what);
        if (ep && ep->ExceptionRecord)
        {
            const EXCEPTION_RECORD* er = ep->ExceptionRecord;
            fprintf(f, "exception:    0x%08lX at %s\n", er->ExceptionCode, ModuleOffset(er->ExceptionAddress).c_str());
            if (er->NumberParameters >= 2 && (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR))
                fprintf(f, "access:       %s 0x%llX\n", er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read",
                        (unsigned long long)er->ExceptionInformation[1]);
        }
        if (details && *details) fprintf(f, "%s%s", details, details[strlen(details) - 1] == '\n' ? "" : "\n");
        fclose(f);
    }
    WriteDump(dir + "\\hc09.dmp", ep);
    if (FILE* f = fopen("crash-reports\\latest.txt", "w"))
    {
        fprintf(f, "%s\n", name);
        fclose(f);
    }
    fprintf(stderr, "[runtime] crash report: %s\n", dir.c_str());
    fflush(stderr);
}

void CrashReportInstall()
{
    SetUnhandledExceptionFilter(OnUnhandled);
    std::set_terminate([] {
        CrashReportWrite("std::terminate (an error the runtime did not handle)", nullptr, nullptr);
        fflush(stderr);
        _exit(3);
    });
}

#else   // other platforms: the log is the report

void CrashReportWrite(const char*, const char*, _EXCEPTION_POINTERS*) {}
void CrashReportInstall() {}

#endif
