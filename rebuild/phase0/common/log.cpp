#include "log.h"
#include "config.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

static std::mutex g_logMutex;
static FILE* g_logFile;

void LogWrite(int level, const char* module, const char* fmt, ...) {
    if (level > GetConfig().logLevel) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile) {
        g_logFile = _wfopen((GameDir() + L"sb2modern.log").c_str(), L"a");
        if (!g_logFile) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_logFile, "%02d:%02d:%02d.%03d [%s] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, module);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}
