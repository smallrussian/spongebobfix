#pragma once

// Appends to sb2modern.log in the game directory. Level 1 = info, 2 = verbose.
void LogWrite(int level, const char* module, const char* fmt, ...);

#define LOG_INFO(fmt, ...)    LogWrite(1, LOG_MODULE, fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(fmt, ...) LogWrite(2, LOG_MODULE, fmt, ##__VA_ARGS__)
// Reports a COM method the game called that is not emulated. Logged once per call site.
#define LOG_UNIMPL(name) do { static bool once_; if (!once_) { once_ = true; LogWrite(1, LOG_MODULE, "UNIMPLEMENTED %s", name); } } while (0)
