**English** · [Русский](webui.ru.md)

# HTML interface in the game (WebUI) — design decision

Goal: modes draw their own phones, inventories, banks and menus in HTML/CSS/JS, like NUI in FiveM and WebView in alt:V / RAGE:MP. The page lives with the mode on the server, the client downloads it and shows it over the game, and the page and Lua exchange messages.

## What was chosen

**WebView2 (Edge, already required by the launcher) in visual hosting mode, with no window on screen; its visual is captured by Windows.Graphics.Capture into a D3D11 texture and drawn into our overlay on the Present hook.**

Verified with a separate probe `src/tools/webprobe.cpp` (the build command is at the top of the file): an 800×600 page with a transparent background arrived as a BGRA frame with alpha (premultiplied): background — alpha 0, a translucent panel — intermediate, text and button — 255. No window is created on screen, and neither the cursor nor the yellow capture border is drawn.

### Why this way

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **WebView2 + visual capture (chosen)** | Already installed for players (the launcher requires it), modern Chromium, nothing to download; the frame is a GPU texture, copied on the GPU; works in any screen mode (we draw inside the game frame) | No official off-screen API: visual + `GraphicsCaptureItem.CreateFromVisual` (Windows 10 1903+); keyboard — via the DevTools Protocol | Yes |
| WebView2 in a transparent window over the game (`WS_EX_NOREDIRECTIONBITMAP` + DComp) | Simpler, no capture | Not visible in exclusive fullscreen; the window takes focus — the game pauses; Steam overlays/video recorders do not see it | No |
| CEF off-screen (like FiveM, alt:V, RAGE:MP) | A real OSR API, the standard in multiplayer mods | ~150 MB of binaries per client, building the wrapper with CMake, security updates are on us | No (possible later, if WebView2 hits a wall) |
| Ultralight | Lightweight, OSR, renders on the GPU | Download, license (free only up to $100k revenue), not full Chromium | No |
| RmlUi / litehtml | Small, draw directly into DX11 | Not real HTML/JS: modders would have to learn a dialect; litehtml has no JS | No |

## How it works

```
server: modes/<mode>/ui/**  ──UiList(name, sha256)──▶ client: cache/ui/<sha256>
                              ◀──UiRequest(name)──────
                              ──UiData(parts)───────▶
Lua  player:ui("phone.html")  ──Ui(open)───────────▶ WebView thread ── https://mode.dgmp/phone.html
Lua  player:uiSend(name, t)    ──Ui(event, json)────▶   dgmp.on(name, fn)
Lua  on("uiMessage", fn)      ◀──UiMessage(name,json)─  dgmp.send(name, data)
```

### Mode files
- Everything from `modes/<mode>/ui/` (with subfolders): html, css, js, json, svg, png, jpg, gif, webp, woff/woff2/ttf/otf, mp3/ogg/wav. Up to 4 MB per file, up to 32 MB per mode.
- Transfer and cache are like images (`images.cpp`): a list with sha256 on join, the client downloads the missing parts in chunks, a cache by hash in `cache/ui/`.
- The page opens on the virtual host `https://mode.dgmp/`; the client serves requests to it from the cache (`WebResourceRequested`). Requests to other hosts are blocked: the mode puts everything it needs into `ui/` (player privacy, works offline, the same for everyone). Navigation away from `mode.dgmp` and new windows are forbidden.

### Rendering (`src/webui.cpp`)
- Its own thread (STA + `DispatcherQueue`): the WebView2 environment (`cache/webview`), a `CompositionController` with a `Windows.UI.Composition.ContainerVisual` root, a hidden parent window.
- Capture: its own D3D11 device on the game's adapter, `Direct3D11CaptureFramePool`; a new frame is copied into a shared texture (keyed mutex). On the Present thread the overlay takes it (a copy into its own texture, if the mutex is free) and draws it across the whole screen through ImGui with premultiplied blending — above the mode's HUD, below dialogs.
- WebView2 starts lazily — on the first `player:ui`: modes without HTML pay nothing (Edge processes ~100–150 MB).
- The page size = the client area of the game window; on a resolution change — new bounds and the frame pool is recreated.

### Input
- `focus = false` (default): the page is only a picture (HTML HUD), the game is controlled as usual.
- `focus = true`: the cursor is visible, the game receives neither mouse nor keyboard (as in an open dialog). Mouse — `ICoreWebView2CompositionController::SendMouseInput` (game window coordinates → page), the cursor shape is taken from the page. Keyboard — the DevTools Protocol `Input.dispatchKeyEvent` (real, `isTrusted`, events: input fields, Enter, Backspace, arrows and text paste work); the page focus is emulated (`Emulation.setFocusEmulationEnabled`).
- An open ImGui dialog takes priority over the page: while it is open, input is theirs.
- The page receives Esc itself; whether to close is up to the mode (as in FiveM).

### Bridge
- The bridge script is injected into every page (`AddScriptToExecuteOnDocumentCreated`): `dgmp.send(name, data)`, `dgmp.on(name, fn)`, `dgmp.focused`.
- Messages are JSON. Page → server: up to 16 KB, up to 30 per second (extras are dropped by the client and the server). Server → page: up to 64 KB.
- The server does not trust the page: `uiMessage` is the same player input as chat.

## Client Lua
Done: [`docs/client-lua.md`](client-lua.md). The scripts in `modes/<mode>/client/` are distributed and cached the same way as the page files. Interface logic can live in the page's JS or in a client script: `onUi(name, fn)` takes `dgmp.send` for itself (it then does not go to the server), `uiSend(name, data)` answers the page.

## Verification
- `src/tools/webui_harness.cpp` — `webui.cpp` without the game: a D3D11 window + ImGui instead of the overlay, the mode files "arrive" as from the server, a small Lua stub answers the page. `--test` clicks by itself, types (Latin and Cyrillic, Backspace, Enter), turns on focus and takes frames `build\harness-*.bmp`. Verified: transparency and translucency (premultiplied alpha), loading css/js/svg from the cache, internet blocking, `isTrusted` clicks and input, the caret in an input field, `dgmp.on`/`dgmp.send` both ways, a repeat run with the file cache and browser profile, a clean process exit.
- In the game (the `htmlui` mode, a 1600×900 window): the browser starts inside the game process, the status panel is drawn over the frame; the phone opens with focus, a click on a button reaches Lua and back ("Lua answered: click 1"), typed text (Latin and Cyrillic) goes to Lua and returns to the game chat, the ✕ button drops focus. For such checks messages are sent to the `UnrealWindow` window (not the process's main window — that is the DGMP HUD window).
- The server and bridge were verified with `dgmp-probe` (`!ui <name> [json]`): the file list, opening, `uiSend`, Lua's reply to a page message, dropping focus.

## Prototype limitations
- One page (layer) per player; several windows live inside one page (SPA), as is done in FiveM.
- IME (entering CJK characters) is not supported; Cyrillic and Latin are (WM_CHAR).
- WebView2 Runtime is required (everyone who has run the launcher has it) and Windows 10 1903+.
