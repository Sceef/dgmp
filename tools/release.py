"""Release a client build so that launchers update themselves (docs/releases.md).

  python tools/release.py --notes-ru "..." --notes-en "..."            # build the ZIP, write update.json (nothing published)
  python tools/release.py --notes-ru "..." --notes-en "..." --publish  # also: GitHub release v<version> with the ZIP,
                                                                        # then commit and push update.json

The ZIP goes to GitHub Releases (binaries are kept out of the git history); update.json in the repository
root says which release is the newest: version, protocol, the ZIP's address, size and SHA-256. The launcher
reads it from raw.githubusercontent.com once per start (src/launcher/main.cpp, kDefaultUpdateUrl).
Order matters: the release with its ZIP exists before update.json points at it.
"""
import sys
from pathlib import Path
# tools/ holds scripts named like standard modules (inspect.py): keep it off the import path.
sys.path = [p for p in sys.path if Path(p or ".").resolve() != Path(__file__).resolve().parent]
import argparse
from hashlib import sha256
import json
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REPO = "Sceef/dgmp"


def run(*args, **kw):
    print("$", " ".join(str(a) for a in args))
    return subprocess.run(args, cwd=ROOT, check=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--notes-ru", default="")
    ap.add_argument("--notes-en", default="")
    ap.add_argument("--publish", action="store_true", help="create the GitHub release and push update.json")
    ap.add_argument("--zip", help="an already built ZIP (default: run tools/package-client.py)")
    a = ap.parse_args()

    version = re.search(r'kVersion\s*=\s*"([^"]+)"', (ROOT / "src/shared/version.h").read_text())[1]
    protocol = int(re.search(r'kProtocolVersion\s*=\s*(\d+)', (ROOT / "src/shared/protocol.h").read_text())[1])
    if a.zip:
        built = Path(a.zip)
    else:
        out = run(sys.executable, "-P", "tools/package-client.py", capture_output=True, text=True).stdout
        print(out, end="")
        built = Path(next(line for line in out.splitlines() if line.strip().endswith(".zip")).strip())
    name = f"DGMP-client-{version}.zip"
    zip_path = ROOT / "build" / "releases" / name
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    if built.resolve() != zip_path.resolve():
        shutil.copy2(built, zip_path)
    data = zip_path.read_bytes()
    tag = "v" + version
    manifest = {
        "version": version,
        "protocol": protocol,
        "zip": f"https://github.com/{REPO}/releases/download/{tag}/{name}",
        "sha256": sha256(data).hexdigest(),
        "bytes": len(data),
        "url": f"https://github.com/{REPO}/releases/tag/{tag}",
        "notes": {"ru": a.notes_ru or f"DGMP {version}", "en": a.notes_en or f"DGMP {version}"},
    }
    (ROOT / "update.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"update.json: {version} (protocol {protocol}), {name} {len(data)} bytes, sha256 {manifest['sha256']}")
    if not a.publish:
        print("Not published (add --publish).")
        return

    notes = ROOT / "build" / "releases" / f"notes-{version}.md"
    # The protocol tells server owners what plays together; the SHA-256 is for the launcher (update.json).
    notes.write_text(f"{manifest['notes']['ru']}\n\n{manifest['notes']['en']}\n\n"
                     f"Протокол {protocol}: играет с серверами того же протокола. / "
                     f"Protocol {protocol}: plays with servers of the same protocol.\n", encoding="utf-8")
    exists = subprocess.run(["gh", "release", "view", tag, "--repo", REPO], cwd=ROOT, capture_output=True).returncode == 0
    if exists:
        # Never another file under a published version: launchers may hold its old size and SHA-256
        # (raw.githubusercontent.com keeps update.json for 5 minutes) and refuse the new file.
        raise SystemExit(f"Release {tag} exists already: raise kVersion in src/shared/version.h for a new build.")
    else:
        run("gh", "release", "create", tag, str(zip_path), "--repo", REPO, "--title", f"DGMP {version}", "--notes-file", str(notes))
    run("git", "add", "update.json")
    if subprocess.run(["git", "diff", "--cached", "--quiet", "update.json"], cwd=ROOT).returncode != 0:
        run("git", "commit", "-m", f"Release {version}: update.json points at {tag}")
    run("git", "push", "origin", "HEAD")
    print(f"Published {tag}; launchers older than {version} offer the update.")


if __name__ == "__main__":
    main()
