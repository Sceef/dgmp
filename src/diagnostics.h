#pragma once
#include <windows.h>
#include <filesystem>

namespace diagnostics {
// Development-only capture. No uploads and no exception swallowing.
void Start(const std::filesystem::path& root, bool enabled);
bool Enabled();
void SetEnabled(bool enabled);
void Phase(const char* literal); // static string, thread local; safe in an exception callback
void Event(const char* format, ...);
void Object(const char* action, const void* address, int index, int serial);
void Heartbeat(double now, int objects, float x, float y, float z);
}
