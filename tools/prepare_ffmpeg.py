"""Restore the locked FFmpeg SDK before CMake configuration; never used by the application."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import zipfile


REPOSITORY = Path(__file__).resolve().parents[1]
STAMP = ".afc-sdk.json"
COMPONENTS = ("avformat", "avcodec", "avfilter", "avutil", "swresample")


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def host_platform():
    if platform.machine().lower() not in ("amd64", "x86_64"):
        raise ValueError("Bundled FFmpeg supports x86_64 only.")
    if sys.platform == "win32":
        return "windows-x64"
    if sys.platform == "linux":
        return "linux-x64"
    raise ValueError("Bundled FFmpeg supports Windows and Linux only.")


def inventory(root):
    return {
        path.relative_to(root).as_posix(): (
            {"link": os.readlink(path)} if path.is_symlink() else {"sha256": sha256(path)}
        )
        for path in sorted(root.rglob("*"))
        if (path.is_symlink() or path.is_file()) and path.name != STAMP
    }


def validate_layout(root, target):
    windows = target == "windows-x64"
    required = [root / "include" / "libavformat" / "avformat.h", root / "LICENSE.txt"]
    required += [root / "bin" / (name + (".exe" if windows else ""))
                 for name in ("ffmpeg", "ffprobe")]
    if windows:
        required += [root / "lib" / (name + ".lib") for name in COMPONENTS]
    else:
        required += [root / "lib" / ("lib" + name + ".so") for name in COMPONENTS]
    for path in required:
        if not path.is_file():
            raise ValueError(f"FFmpeg SDK is incomplete: {path}")
    if windows and not list((root / "bin").glob("*.dll")):
        raise ValueError("FFmpeg SDK has no runtime DLLs.")


def verify(root, identity):
    validate_layout(root, identity["platform"])
    stamp = json.loads((root / STAMP).read_text(encoding="utf-8"))
    if stamp.get("identity") != identity or stamp.get("files") != inventory(root):
        raise ValueError(f"FFmpeg SDK differs from its verified restore: {root}")


def safe_name(name):
    path = PurePosixPath(name)
    if "\\" in name or ":" in name or path.is_absolute() or ".." in path.parts:
        raise ValueError(f"Unsafe SDK archive member: {name}")
    return path


def extract(archive, destination):
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as package:
            for member in package.infolist():
                safe_name(member.filename)
                if (member.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError("SDK ZIP symlinks are not supported.")
            package.extractall(destination)
    else:
        with tarfile.open(archive, "r:xz") as package:
            for member in package.getmembers():
                safe_name(member.name)
                if member.issym() or member.islnk():
                    # FFmpeg SONAME links are relative, within the SDK's lib directory.
                    safe_name(member.linkname)
                elif not (member.isdir() or member.isfile()):
                    raise ValueError(f"Unsupported SDK archive member: {member.name}")
            package.extractall(destination, filter="data")


def prepare(entry, identity, root, cache, offline=False):
    if root.exists():
        verify(root, identity)
        return
    archive = cache / entry["sha256"] / Path(entry["url"]).name
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        if offline:
            raise ValueError(f"Offline FFmpeg archive not found: {archive}")
        with tempfile.TemporaryDirectory(prefix="download-", dir=archive.parent) as temporary:
            downloaded = Path(temporary) / "archive"
            with urllib.request.urlopen(entry["url"], timeout=60) as response, downloaded.open("wb") as output:
                shutil.copyfileobj(response, output)
            if sha256(downloaded) != entry["sha256"]:
                raise ValueError("FFmpeg archive SHA-256 mismatch; nothing was installed.")
            downloaded.replace(archive)
    if sha256(archive) != entry["sha256"]:
        raise ValueError(f"FFmpeg archive SHA-256 mismatch: {archive}")
    root.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".ffmpeg-", dir=root.parent) as temporary:
        unpacked = Path(temporary)
        extract(archive, unpacked)
        children = list(unpacked.iterdir())
        if len(children) != 1 or not children[0].is_dir():
            raise ValueError("FFmpeg archive must contain exactly one SDK directory.")
        sdk = children[0]
        validate_layout(sdk, identity["platform"])
        (sdk / STAMP).write_text(json.dumps({
            "identity": identity, "files": inventory(sdk)
        }, indent=2) + "\n", encoding="utf-8")
        sdk.rename(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("windows-x64", "linux-x64"))
    parser.add_argument("--root", type=Path, help="Explicit SDK destination (also used by --verify-only).")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    try:
        lock = json.loads((REPOSITORY / "cmake" / "ffmpeg-lock.json").read_text(encoding="utf-8"))
        target = args.platform or host_platform()
        entry = lock["platforms"][target]
        identity = {"build": lock["build"], "variant": lock["variant"],
                    "platform": target, "sha256": entry["sha256"]}
        root = (args.root or REPOSITORY / "external" / "ffmpeg" / lock["build"] / target).resolve()
        if args.verify_only:
            verify(root, identity)
        else:
            prepare(entry, identity, root, REPOSITORY / "out" / "dependencies" / "ffmpeg", args.offline)
        print(f"Verified FFmpeg SDK: {root}")
        return 0
    except (OSError, ValueError, KeyError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"FFmpeg setup failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
