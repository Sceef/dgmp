#pragma once
#include <string>
namespace profile {
bool Install();
const std::wstring& Root();
const std::wstring& Saved();
int PatchedCount();
unsigned int BlockedSaveWrites();
}
