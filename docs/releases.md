**English** · [Русский](releases.ru.md)

# Releasing a version and updating from the launcher

Client builds are stored not in git but in **GitHub Releases**: `DGMP-client-0.8.0.zip` is attached to the tag `v0.8.0`. The repository root contains a small file [`update.json`](../update.json): it tells launchers which version is the latest and where to get it.

```json
{
  "version": "0.8.0",
  "protocol": 11,
  "zip": "https://github.com/Sceef/dgmp/releases/download/v0.8.0/DGMP-client-0.8.0.zip",
  "sha256": "…",
  "bytes": 2933717,
  "url": "https://github.com/Sceef/dgmp/releases/tag/v0.8.0",
  "notes": { "ru": "…", "en": "…" }
}
```

## How the launcher updates

1. On every start `DGMP.exe` reads `https://raw.githubusercontent.com/Sceef/dgmp/main/update.json`. You can set your own address in `profiles/launcher.json` → `"updateUrl"`; an empty string disables the check.
2. If `version` is newer than its own, "Version X is out — update" appears on the left.
3. On click the launcher:
   - downloads the ZIP and checks the size and SHA-256 against the manifest;
   - unpacks the archive with the `tar` built into Windows and checks that `build-info.json` has the same version;
   - copies the files over its folder.

   The player's `DGMP.ini` and `editor.ini` are not replaced; `profiles/` and `runtime/` are not in the archive. The running `DGMP.exe` is renamed to `DGMP.exe.old`: the next start deletes it.
4. The new launcher starts by itself. The new `version.dll` goes into `runtime/` on the next entry into the game (`prepare.ps1`).

While Days Gone or your own server from the launcher is running, the update is not installed: the launcher asks you to close them. If the manifest has no `zip` and `sha256`, the button just opens the `url` page.

Downloading without a GitHub login is possible only from a **public** repository. While it is private, the update check silently finds nothing.

## How to release a version

1. Bump `kVersion` in [`src/shared/version.h`](../src/shared/version.h). If network messages changed, also bump `kProtocolVersion` in `src/shared/protocol.h`: clients and servers of different protocols do not play together, so update the server too.
2. Build: `build.bat`.
3. Check the build: `python tests/outpost_smoke.py`, `build/lua-test.exe tests/outpost.lua`, the game.
4. Commit and push the changes.
5. Release:

   ```
   python tools/release.py --notes-ru "Что нового" --notes-en "What is new" --publish
   ```

   The script:
   - builds the ZIP (`tools/package-client.py`) and copies it to `build/releases/DGMP-client-<version>.zip`;
   - writes `update.json`;
   - creates the release `v<version>` with the ZIP via `gh` (if the release already exists, replaces the file);
   - commits and pushes `update.json`.

   Without `--publish` it only builds the ZIP and writes `update.json`, publishing nothing.
6. Update your servers (for example, a VPS: [`docs/server-linux.md`](server-linux.md)).

The order matters: first the release with the ZIP, then `update.json`, otherwise launchers get a link to a non-existent file. `release.py` does exactly that. A different ZIP is not uploaded under an already published version: launchers with a cached `update.json` know the size and SHA-256 of the old file and will refuse to install the new one. A new version is needed. Right after a release, launchers may see the previous `update.json` for up to 5 minutes (the `raw.githubusercontent.com` cache).
