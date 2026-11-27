"""Exercise the copied application's default runtime, not an injected SDK encoder."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def run(*arguments, **kwargs):
    return subprocess.run([str(arg) for arg in arguments], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=45, **kwargs)


class BundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="afc-bundle-")
        cls.root = Path(cls.temporary.name)
        cls.bundle = cls.root / "relocated bundle-\u97f3" / "bin"
        cls.bundle.mkdir(parents=True)
        cls.original_encoder = ARGS.converter.parent / ("ffmpeg.exe" if os.name == "nt" else "ffmpeg")
        for name in [ARGS.converter.name, cls.original_encoder.name,
                     "ffprobe.exe" if os.name == "nt" else "ffprobe"]:
            shutil.copy2(ARGS.converter.parent / name, cls.bundle / name)
        if os.name == "nt":
            ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
            for library in ARGS.converter.parent.glob("*.dll"):
                shutil.copy2(library, cls.bundle / library.name)
        else:
            shutil.copytree(ARGS.converter.parent.parent / "lib" / "AudioLibraryToolkit",
                            cls.bundle.parent / "lib" / "AudioLibraryToolkit", symlinks=True)
        cls.application = cls.bundle / ARGS.converter.name
        cls.source = cls.root / "source"
        cls.source.mkdir()
        cls.track = cls.source / "track.flac"
        result = run(cls.original_encoder, "-v", "error", "-f", "lavfi", "-i",
                     "anullsrc=r=44100:cl=stereo", "-t", "0.1", "-c:a", "flac", cls.track)
        if result.returncode:
            raise RuntimeError(result.stderr)
        cls.original = cls.track.read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.work = self.root / self._testMethodName
        self.work.mkdir()
        self.config = self.work / "config.json"
        self.config.write_text("{}", encoding="utf-8")
        self.output = self.work / "output"
        self.env = os.environ.copy()
        self.env["LOCALAPPDATA"] = str(self.work / "local")
        self.env["XDG_CONFIG_HOME"] = str(self.work / "config-home")
        self.env["XDG_STATE_HOME"] = str(self.work / "state")
        self.env["PATH"] = ""

    def tearDown(self):
        self.assertEqual(self.track.read_bytes(), self.original)

    def convert(self):
        return run(self.application, "convert", self.source, "--to", "mp3", "--config", self.config,
                   "--output-dir", self.output, cwd=self.work, env=self.env)

    def test_relocated_default_ignores_host_encoder(self):
        # This would reject conversion if the working-directory/PATH executable were selected.
        shutil.copy2(self.bundle / ("ffprobe.exe" if os.name == "nt" else "ffprobe"),
                     self.work / self.original_encoder.name)
        self.env["PATH"] = str(self.work)
        result = self.convert()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FFmpeg encoder (bundled)", result.stdout)
        self.assertIn(str(self.bundle / self.original_encoder.name), result.stdout)
        self.assertTrue(list(self.output.rglob("*.mp3")))

    def test_missing_bundle_never_uses_available_host_encoder(self):
        executable = self.bundle / self.original_encoder.name
        saved = executable.with_suffix(".saved")
        executable.rename(saved)
        self.env["PATH"] = str(self.original_encoder.parent)
        try:
            result = self.convert()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("No fallback will be used", result.stdout + result.stderr)
            self.assertFalse(self.output.exists())
        finally:
            saved.rename(executable)

    def test_absolute_override_warns(self):
        self.config.write_text(json.dumps({"FLACSettings": {
            "ffmpeg_exe_name": str(self.original_encoder)}}), encoding="utf-8")
        result = self.convert()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("External FFmpeg override", result.stdout + result.stderr)

    def test_non_encoder_override_fails_before_writes(self):
        self.config.write_text(json.dumps({"FLACSettings": {
            "ffmpeg_exe_name": str(self.bundle / ("ffprobe.exe" if os.name == "nt" else "ffprobe"))}}))
        result = self.convert()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not an FFmpeg encoder", result.stdout + result.stderr)
        self.assertFalse(self.output.exists())

    def test_worker_stdout_stays_protocol_only(self):
        request = {"protocolVersion": 1, "requestId": "bundled-conversion", "type": "run",
                   "operation": "convert", "parameters": {
                       "inputPath": str(self.source), "target": "mp3",
                       "outputDirectory": str(self.output), "configPath": str(self.config)}}
        result = run(self.application, "--worker", input=json.dumps(request) + "\n",
                     cwd=self.work, env=self.env)
        self.assertEqual(result.returncode, 0, result.stderr)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(events)
        self.assertEqual(events[0]["type"], "ready")
        completed = next(event for event in events if event["type"] == "completed")
        self.assertEqual(completed["status"], "succeeded", events)
        self.assertIn("FFmpeg encoder (bundled)", result.stderr)
        self.assertTrue(list(self.output.rglob("*.mp3")))

    def test_menu_uses_bundle_without_path(self):
        self.config.write_text(json.dumps({
            "OutDirectory": str(self.output), "MediaDirectoryList": [{
                "isActive": True, "mediaPath": str(self.source), "mediaName": "Library"}]
        }), encoding="utf-8")
        config_home = Path(self.env["LOCALAPPDATA"] if os.name == "nt" else self.env["XDG_CONFIG_HOME"])
        user_config = config_home / "AudioLibraryToolkit" / "config.json"
        user_config.parent.mkdir(parents=True)
        shutil.copy2(self.config, user_config)
        result = run(self.application, input="1\nRUN\n", cwd=self.work, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("FFmpeg encoder (bundled)", result.stdout)
        self.assertTrue(list(self.output.rglob("*.flac")))

    def test_external_library_is_rejected_even_when_versions_match(self):
        if os.name == "nt":
            library = next(self.bundle.glob("avformat-*.dll"))
            self.env["PATH"] = str(ARGS.converter.parent)
        else:
            library = next(path for path in (self.bundle.parent / "lib" / "AudioLibraryToolkit").glob(
                "libavformat.so.*") if not path.is_symlink())
            self.env["LD_LIBRARY_PATH"] = str(ARGS.converter.parent.parent / "lib" / "AudioLibraryToolkit")
        saved = library.with_name(library.name + ".saved")
        library.rename(saved)
        try:
            result = run(self.application, "--help", cwd=self.work, env=self.env)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("external or incompatible", result.stdout + result.stderr)
        finally:
            saved.rename(library)

    def test_missing_library_fails_at_startup(self):
        if os.name == "nt":
            library = next(self.bundle.glob("avformat-*.dll"))
        else:
            library = next(path for path in (self.bundle.parent / "lib" / "AudioLibraryToolkit").glob(
                "libavformat.so.*") if not path.is_symlink())
        saved = library.with_name(library.name + ".saved")
        library.rename(saved)
        try:
            result = run(self.application, "--help", cwd=self.work, env=self.env)
            self.assertNotEqual(result.returncode, 0)
        finally:
            saved.rename(library)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--converter", type=lambda path: Path(path).resolve(), required=True)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
