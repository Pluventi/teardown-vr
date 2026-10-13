#pragma once

// Simple thread-safe logger writing to TeardownVR.log next to the game exe.
// Lines are formatted on the calling thread and written by a writer thread (every 50 ms, or
// sooner when 64 KB wait), so a game thread never waits for the disk. LogFlush writes what
// waits at once, on the calling thread (hang, LogStack, DLL unload); a fatal-kind exception
// writes it too, but only if the log is free at that instant (it may be handled: never a wait
// there). With [Debug] logSync=1 every line is written by the calling thread, as before.
void LogInit();
// After the ini is read: async = the writer thread (logSync=0), or every line written at once.
void LogConfigure(bool async);
void Log(const char* fmt, ...);
// Writes the lines still waiting, on the calling thread. Never waits more than ~0.2 s (0.1 s per
// lock, measured) for a thread that holds the log (a crashing or suspended thread): what it holds
// is then not written by this call.
void LogFlush();
// DLL unload / process exit: the last lines, without waiting for the writer thread.
void LogShutdown();
// Logs the call stack as offsets inside teardown.exe (reverse engineering aid).
void LogStack(const char* tag);
