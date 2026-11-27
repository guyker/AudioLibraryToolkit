import importlib.util
from pathlib import Path
import sys
import tempfile
import tarfile
import unittest
import zipfile


spec = importlib.util.spec_from_file_location(
    "prepare_ffmpeg", Path(__file__).resolve().parents[1] / "tools" / "prepare_ffmpeg.py")
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from package_application import redistribution_sources


class SetupTests(unittest.TestCase):
    def test_restore_offline_cache_and_changed_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            archive = base / "sdk.zip"
            with zipfile.ZipFile(archive, "w") as package:
                for name in ["LICENSE.txt", "include/libavformat/avformat.h", "bin/ffmpeg.exe", "bin/ffprobe.exe",
                             "bin/avformat-1.dll"] + ["lib/" + c + ".lib" for c in setup.COMPONENTS]:
                    package.writestr("sdk/" + name, "fixture")
            entry = {"url": archive.as_uri(), "sha256": setup.sha256(archive)}
            identity = {"platform": "windows-x64", "sha256": entry["sha256"]}
            root = base / "restored"
            setup.prepare(entry, identity, root, base / "cache")
            archive.unlink()
            setup.prepare(entry, identity, root, base / "cache", offline=True)
            second = base / "second"
            setup.prepare(entry, identity, second, base / "cache", offline=True)
            (root / "bin" / "ffmpeg.exe").write_text("changed")
            with self.assertRaisesRegex(ValueError, "differs"):
                setup.verify(root, identity)

    def test_bad_hash_does_not_install(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            archive = base / "sdk.zip"
            archive.write_bytes(b"not the approved SDK")
            root = base / "restored"
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                setup.prepare({"url": archive.as_uri(), "sha256": "0" * 64},
                              {"platform": "windows-x64"}, root, base / "cache")
            self.assertFalse(root.exists())

    def test_unsafe_archive_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            archive = base / "sdk.zip"
            with zipfile.ZipFile(archive, "w") as package:
                package.writestr("../escaped", "bad")
            with self.assertRaisesRegex(ValueError, "Unsafe"):
                setup.extract(archive, base / "unpacked")
            self.assertFalse((base / "escaped").exists())

    def test_incomplete_or_truncated_archive_does_not_install(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            archive = base / "sdk.zip"
            with zipfile.ZipFile(archive, "w") as package:
                package.writestr("sdk/LICENSE.txt", "fixture")
            root = base / "restored"
            identity = {"platform": "windows-x64"}
            with self.assertRaisesRegex(ValueError, "SDK is incomplete"):
                setup.prepare({"url": archive.as_uri(), "sha256": setup.sha256(archive)},
                              identity, root, base / "cache")
            self.assertFalse(root.exists())
            archive.write_bytes(archive.read_bytes()[:20])
            with self.assertRaises(tarfile.ReadError):
                setup.prepare({"url": archive.as_uri(), "sha256": setup.sha256(archive)},
                              identity, root, base / "cache")
            self.assertFalse(root.exists())

    def test_release_requires_reviewed_matching_sources(self):
        with self.assertRaisesRegex(ValueError, "Redistribution blocked"):
            redistribution_sources({"build": "pinned"}, {"build": "pinned", "status": "pending"})
        with self.assertRaisesRegex(ValueError, "no matching corresponding-source"):
            redistribution_sources({"build": "pinned"}, {"build": "pinned", "status": "reviewed"})
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "sources.tar.gz"
            source.write_bytes(b"test source archive")
            record = {"build": "pinned", "status": "reviewed", "sourceArchives": [
                {"file": str(source), "sha256": setup.sha256(source)}]}
            self.assertEqual(redistribution_sources({"build": "pinned"}, record), [source])
            source.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "missing or changed source archive"):
                redistribution_sources({"build": "pinned"}, record)


if __name__ == "__main__":
    unittest.main()
