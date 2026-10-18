#pragma once
// The OS-specific pieces of dgmp-server, so the rest builds as it is with MSVC on Windows and with
// g++/clang on Linux.
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

namespace srv {

std::filesystem::path ExecutableDir();                     // the server's root: server.ini, modes/, data/, logs/
std::filesystem::path Utf8Path(const std::string& utf8);
FILE* OpenAppend(const std::filesystem::path& file);
std::tm LocalTime(std::time_t t);
int StrICmp(const char* a, const char* b);                  // ASCII case-insensitive, like _stricmp / strcasecmp
// `key` of [section] in an .ini file, as UTF-8. An empty value stays empty; `fallback` only when the key is
// missing. Windows reads the file with GetPrivateProfileString; elsewhere IniParse does the same on its text.
std::string IniValue(const std::filesystem::path& file, const char* section, const char* key, const char* fallback = "");
std::string IniParse(std::string text, const char* section, const char* key, const char* fallback = "");
std::string Sha256Hex(const void* data, size_t size);       // lowercase hex digest
#ifdef _WIN32
std::string Utf8(const std::wstring& w);
#endif

}  // namespace srv
