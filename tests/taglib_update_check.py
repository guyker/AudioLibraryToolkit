import json
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


CMAKE = sys.argv.pop(1)
SCRIPT = sys.argv.pop(1)


class ReleaseHandler(BaseHTTPRequestHandler):
    status = 200
    body = b""

    def do_GET(self):
        self.send_response(self.status)
        self.end_headers()
        self.wfile.write(self.body)

    def log_message(self, format, *args):
        pass


class TagLibUpdateCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = HTTPServer(("127.0.0.1", 0), ReleaseHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = f"http://127.0.0.1:{cls.server.server_port}/release"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.thread.join()
        cls.server.server_close()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        header = self.root / "source" / "taglib" / "toolkit" / "taglib.h"
        header.parent.mkdir(parents=True)
        header.write_text(
            "#define TAGLIB_MAJOR_VERSION 2\n"
            "#define TAGLIB_MINOR_VERSION 1\n"
            "#define TAGLIB_PATCH_VERSION 1\n",
            encoding="utf-8",
        )
        ReleaseHandler.status = 200
        ReleaseHandler.body = b""

    def run_check(self, url=None):
        result = subprocess.run(
            [
                CMAKE,
                f"-DTAGLIB_SOURCE_DIR={self.root / 'source'}",
                f"-DTAGLIB_UPDATE_TMP_DIR={self.root}",
                f"-DTAGLIB_RELEASE_URL={url or self.url}",
                "-P",
                SCRIPT,
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(self.root.glob("taglib-release-*.json")), [])
        return result.stdout + result.stderr

    def test_newer_stable_release_is_reported(self):
        ReleaseHandler.body = json.dumps({"tag_name": "v2.3.2"}).encode()
        output = self.run_check()
        self.assertIn("TagLib update available", output)
        self.assertIn("using 2.1.1; latest stable is 2.3.2", output)
        self.assertIn("releases/tag/v2.3.2", output)
        self.assertIn("pinned version is unchanged", output)
        self.assertIn("this notice does not stop the build", output)
        self.assertIn("build command's exit code (0 = success)", output)

    def test_equal_or_older_release_does_not_report_an_update(self):
        for tag in ("v2.1.1", "v2.1", "v2.0.9"):
            with self.subTest(tag=tag):
                ReleaseHandler.body = json.dumps({"tag_name": tag}).encode()
                output = self.run_check()
                self.assertIn("TagLib update check: using 2.1.1", output)
                self.assertNotIn("update available", output)

    def test_network_or_http_error_does_not_fail_build(self):
        ReleaseHandler.status = 403
        output = self.run_check()
        self.assertIn("TagLib update check unavailable", output)
        self.assertIn("Build continues", output)
        output = self.run_check("http://127.0.0.1:0/release")
        self.assertIn("TagLib update check unavailable", output)

    def test_invalid_response_is_reported_without_failing(self):
        for body in (b"not json", b'{"tag_name":"v3.0.0-rc1"}', b"{}"):
            with self.subTest(body=body):
                ReleaseHandler.body = body
                output = self.run_check()
                self.assertIn("unrecognized release response", output)
                self.assertNotIn("update available", output)

    def test_missing_current_version_does_not_fail_build(self):
        (self.root / "source" / "taglib" / "toolkit" / "taglib.h").unlink()
        self.assertIn("version header not found", self.run_check())


if __name__ == "__main__":
    unittest.main()
