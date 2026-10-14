#pragma once
// In-game UI drawn into the game's own frames: Dear ImGui on the game's Direct3D 11 swap chain
// (IDXGISwapChain::Present is hooked). Used by the loading cover and the editor's tool windows.
//
// Threads: the draw callbacks run on the thread that presents frames (not the game thread). They
// must not call into the engine; they read/write their own state under their own locks.
namespace overlay {

// Hooks the swap chain (any time, also before the game created its device). Safe to call more than once.
bool Install();

// Shown at all, and whether the mouse belongs to the UI (cursor mode, toggled with Tab while shown).
void SetVisible(bool visible);
bool Visible();
void SetCursorMode(bool on);
bool CursorMode();
// The UI holds the keyboard (a text field is focused) / the mouse is over a UI window.
bool Typing();
bool MouseOverUi();

// Builds the ImGui windows of a frame (between NewFrame and Render).
using DrawFn = void (*)();
void SetDraw(DrawFn draw);
// Drawn in every frame before the windows, whether the UI is shown or not (the loading cover).
void SetCover(DrawFn draw);
// The game presented a frame lately (false while it is minimized or before its first frame).
bool Presenting();

// Inside a draw callback: a texture from 32-bit BGRA pixels (an ImTextureID), and the large font
// (an ImFont*, null until the UI is set up).
void* CreateTexture(const void* bgra, int width, int height);
void* LargeFont();

// The game never hears that its window lost focus (no pause on alt-tab): for the editor.
void SetKeepFocus(bool keep);

}  // namespace overlay
