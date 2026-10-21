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
// HTTPS (or HTTP) GET; blocks up to `timeoutSec`, so call it off the server thread. True: status 200.
// `body` holds the answer also for other statuses; `error` says what went wrong. WinHTTP on Windows,
// libcurl (loaded at run time: libcurl.so.4) elsewhere.
bool HttpGet(const std::string& url, std::string& body, std::string& error, int timeoutSec);
// Any method (GET, POST, PUT, DELETE...) with a body; `status` is the HTTP status (0: no answer). True: 2xx.
bool HttpRequest(const std::string& method, const std::string& url, const std::string& requestBody, const std::string& contentType,
                 std::string& body, int& status, std::string& error, int timeoutSec);
#ifdef _WIN32
std::string Utf8(const std::wstring& w);
#endif

}  // namespace srv
