#include "steam.h"
#include "log.h"
#include <windows.h>

namespace steam {
namespace {
using UserFn = void*(__cdecl*)();
using GetSteamIdFn = uint64_t(__cdecl*)(void* self);
using GetTicketFn = uint32_t(__cdecl*)(void* self, void* ticket, int maxBytes, uint32_t* bytes);
using CancelFn = void(__cdecl*)(void* self, uint32_t handle);

uint32_t ticketHandle = 0;  // k_HAuthTicketInvalid = 0

// ISteamUser of the game's Steam client, or null.
void* User() {
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    auto user = dll ? reinterpret_cast<UserFn>(GetProcAddress(dll, "SteamAPI_SteamUser_v020")) : nullptr;
    return user ? user() : nullptr;
}
template <typename F> F Export(const char* name) {
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    return dll ? reinterpret_cast<F>(GetProcAddress(dll, name)) : nullptr;
}
}  // namespace

bool Identity(uint64_t& steamId) {
    void* user = User(); auto get = Export<GetSteamIdFn>("SteamAPI_ISteamUser_GetSteamID");
    steamId = user && get ? get(user) : 0;
    return steamId != 0;
}

bool NewTicket(std::string& hex) {
    hex.clear();
    void* user = User(); auto get = Export<GetTicketFn>("SteamAPI_ISteamUser_GetAuthSessionTicket");
    if (!user || !get) return false;
    CancelTicket();
    unsigned char ticket[1024]; uint32_t size = 0;
    ticketHandle = get(user, ticket, sizeof(ticket), &size);
    if (!ticketHandle || !size || size > sizeof(ticket)) { dg::Log("Steam: no auth ticket"); ticketHandle = 0; return false; }
    static const char digits[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < size; ++i) { hex += digits[ticket[i] >> 4]; hex += digits[ticket[i] & 15]; }
    dg::Log("Steam: auth ticket %u (%u bytes)", ticketHandle, size);
    return true;
}

void CancelTicket() {
    if (!ticketHandle) return;
    void* user = User(); auto cancel = Export<CancelFn>("SteamAPI_ISteamUser_CancelAuthTicket");
    if (user && cancel) cancel(user, ticketHandle);
    ticketHandle = 0;
}

}  // namespace steam
