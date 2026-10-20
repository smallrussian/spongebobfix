#pragma once
#include <windows.h>
#include <cstddef>

// Replaces the import-table entry `dll!func` in `module`. Returns the previous target, or nullptr
// if the module does not import it.
void* HookImport(HMODULE module, const char* dll, const char* func, void* replacement);

// Overwrites code bytes after checking they match `expected` (mask byte '?' in `mask` skips a
// position). Returns false and leaves memory untouched on mismatch.
bool PatchBytes(void* address, const unsigned char* expected, const char* mask,
                const unsigned char* replacement, size_t length);

// Redirects the function at `target` to `detour`. `stolenBytes` must cover whole instructions
// (at least 5 bytes) that are position-independent. Returns a callable trampoline to the original.
void* InstallDetour(void* target, void* detour, size_t stolenBytes);
