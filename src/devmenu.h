#pragma once
#include <string>
#include <vector>
namespace devmenu {
struct Snapshot {
    int selected=0;
    std::vector<std::wstring> rows;
    std::wstring status;
    bool error=false;
};
bool IsOpen();
void Update(bool ready);
void Key(int virtualKey);
void Click(int row,int direction);
Snapshot GetSnapshot();
}
