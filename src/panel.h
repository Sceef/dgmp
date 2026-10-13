#pragma once
#include <string>
namespace panel {
void Start();
void Text(const std::wstring& text);
void Loading(bool visible,float progress,const std::wstring& stage);
bool IsLoading();
}
