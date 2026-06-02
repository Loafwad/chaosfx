#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>

// Simple synchronous file logger.
// Writes to %TEMP%\chaosfx.log
// All functions are thread-safe (each write opens+closes the file).
namespace chaosfx::log {

inline void write(const char* fmt, ...)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("TEMP", path, MAX_PATH)) return;
    strncat_s(path, "\\chaosfx.log", MAX_PATH - strlen(path) - 1);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;

    fprintf(f, "[%010llu] ", (unsigned long long)GetTickCount64());

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
}

inline void begin_session()
{
    write("========== ChaosFX standalone loaded ==========");
}

} // namespace chaosfx::log

#define CFXLOG(fmt, ...) chaosfx::log::write("[%s:%d] " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define CFXLOG_IF(cond, fmt, ...) do { if (cond) CFXLOG(fmt, ##__VA_ARGS__); } while(0)
