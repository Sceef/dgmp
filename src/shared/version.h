#pragma once
// One DGMP version for the game client, the launcher and the server (shown in the launcher, sent in
// server status answers, written to the logs). The network protocol has its own number
// (net::kProtocolVersion): servers and clients of different protocols cannot play together.
namespace dgmp {
constexpr const char* kVersion = "0.8.1";
}
