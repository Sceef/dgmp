#pragma once
#include "../api/dgmp.h"
#include <string>
#include <vector>
namespace environment {
struct Snapshot {
    DGMP_EnvironmentState state{};
    std::vector<DGMP_WeatherInfo> weather;
    std::string message="Waiting for world";
};
// Called only by the game thread; public exports only enqueue or read snapshots.
void Update(bool ready);
Snapshot GetSnapshot();
}
