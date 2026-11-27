"""Package a clean CMake installation; release output requires reviewed corresponding-source materials."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from prepare_ffmpeg import REPOSITORY, inventory, sha256


def redistribution_sources(lock, record):
    if record.get("build") != lock["build"] or record.get("status") != "reviewed":
        raise ValueError("Redistribution blocked: " + record.get("reason", "source/license review is incomplete."))
    sources = record.get("sourceArchives", [])
    if not sources:
        raise ValueError("Redistribution blocked: no matching corresponding-source archives are recorded.")
    paths = []
    for source in sources:
        path = (REPOSITORY / source["file"]).resolve()
        if not path.is_file() or sha256(path) != source["sha256"]:
            raise ValueError(f"Redistribution blocked: missing or changed source archive: {path}")
        paths.append(path)
    if len({path.name for path in paths}) != len(paths):
        raise ValueError("Redistribution blocked: source archive names must be distinct.")
    return paths


def package(build, destination, development):
    lock = json.loads((REPOSITORY / "cmake" / "ffmpeg-lock.json").read_text(encoding="utf-8"))
    record = json.loads((REPOSITORY / "cmake" / "ffmpeg-redistribution.json").read_text(encoding="utf-8"))
    sources = [] if development else redistribution_sources(lock, record)
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.startswith(("//", "#")):
            name, value = line.split("=", 1)
            cache[name.split(":", 1)[0]] = value
    if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != REPOSITORY:
        raise ValueError("The build does not belong to this AudioLibraryToolkit checkout.")
    if not development and cache.get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("Redistributable packages require a Release build.")
    destination.mkdir(parents=True, exist_ok=True)
    target = "windows-x64" if os.name == "nt" else "linux-x64"
    name = f"AudioLibraryToolkit-{lock['build']}-{target}" + ("-development" if development else "")
    with tempfile.TemporaryDirectory(prefix=".package-", dir=destination) as temporary:
        tree = Path(temporary) / name
        subprocess.run(["cmake", "--install", str(build), "--prefix", str(tree),
                        "--component", "AudioLibraryToolkit"], check=True)
        exe = tree / "bin" / ("AudioLibraryToolkit.exe" if os.name == "nt" else "AudioLibraryToolkit")
        subprocess.run([str(exe), "--version"], check=True, capture_output=True, timeout=30)
        encoder = exe.parent / ("ffmpeg.exe" if os.name == "nt" else "ffmpeg")
        version = subprocess.run([str(encoder), "-version"], check=True, capture_output=True,
                                 text=True, timeout=30).stdout
        if not version.startswith("ffmpeg version " + lock["build"]):
            raise ValueError("Packaged encoder does not match the locked build.")
        manifest = {
            "build": lock["build"], "platform": target, "developmentOnly": development,
            "redistribution": record, "files": inventory(tree)
        }
        (tree / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        archive = shutil.make_archive(str(Path(temporary) / name),
                                     "zip" if os.name == "nt" else "gztar", temporary, name)
        output = destination / Path(archive).name
        if output.exists():
            raise ValueError(f"Package already exists; choose a new output directory: {output}")
        for source in sources:
            target_source = destination / source.name
            if target_source.exists() and sha256(target_source) != sha256(source):
                raise ValueError(f"Refusing to overwrite a different source artifact: {target_source}")
            if not target_source.exists():
                shutil.copy2(source, target_source)
        Path(archive).rename(output)
        print(f"Created {'DEVELOPMENT-ONLY ' if development else ''}package: {output}")
        return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=REPOSITORY / "out" / "packages")
    parser.add_argument("--development", action="store_true",
                        help="Create a visibly marked local/test artifact, NOT cleared for redistribution.")
    args = parser.parse_args()
    try:
        package(args.build_dir.resolve(), args.output_dir.resolve(), args.development)
        return 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"Packaging failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
