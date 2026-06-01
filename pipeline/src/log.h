#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>

// Simple synchronous file logger.
// Writes to %USERPROFILE%\OpenplanetNext\Plugins\ChaosFX\chaosfx.log
// All functions are thread-safe (each write opens+closes the file).
namespace chaosfx::log {

inline void write(const char* fmt, ...)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("USERPROFILE", path, MAX_PATH)) return;
    strncat_s(path, "\\OpenplanetNext\\Plugins\\ChaosFX\\chaosfx.log", MAX_PATH - strlen(path) - 1);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;

    // Timestamp via GetTickCount64 (ms since boot — good enough for ordering)
    fprintf(f, "[%010llu] ", (unsigned long long)GetTickCount64());

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
}

// Call once at startup to stamp a separator in the log
inline void begin_session()
{
    write("========== ChaosFX DLL loaded ==========");
}

} // namespace chaosfx::log

// Convenience macro — writes file:line prefix automatically
#define CFXLOG(fmt, ...) chaosfx::log::write("[%s:%d] " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define CFXLOG_IF(cond, fmt, ...) do { if (cond) CFXLOG(fmt, ##__VA_ARGS__); } while(0)
