#pragma once
// The master server: the public list of DGMP servers (dgmp-server --master [--port 7700]).
// Game servers with Master= in server.ini send a heartbeat from their game port every minute; the
// master asks each one the usual status query itself (only servers that answer are listed, so nobody
// can list a fake address) and hands the list to launchers. See docs/master.md.
#include <atomic>
#include <cstdint>
#include <filesystem>

namespace srv {
int RunMaster(uint16_t port, const std::filesystem::path& root, const std::atomic<bool>& quit);
}
