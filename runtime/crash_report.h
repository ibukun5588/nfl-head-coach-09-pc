#pragma once
// The crash logger (docs/HANDOFF_M11.md step 5; 2026-10-09/23): on a fatal error the runtime writes a report folder,
// crash-reports\<date-time>\ in hc09.exe's working folder (the install folder when the launcher starts it):
//   crash.txt   build, game version, the error, where (guest function, host module + offset, thread)
//   hc09.dmp    a small minidump: thread stacks, registers and the module list; never a full memory dump (it would
//               hold the game's code and data)
// and crash-reports\latest.txt naming it. The launcher adds the log's tail, hc09.ini and the system's description,
// zips it and offers to open the folder or a prefilled GitHub issue; sending it stays the player's choice.
struct _EXCEPTION_POINTERS;

// Once per process (later calls do nothing); `details`: more lines for crash.txt (may be null); `ep`: the faulting
// context for the minidump (null: the dump shows this thread where it is).
void CrashReportWrite(const char* what, const char* details, _EXCEPTION_POINTERS* ep);

// The backstops: exceptions no one handles, std::terminate. Call once at start.
void CrashReportInstall();
