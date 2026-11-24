import importlib.util
import tempfile
import unittest
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "module_boundaries", Path(__file__).resolve().parents[1] / "check_module_boundaries.py"
)
boundaries = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boundaries)


class ModuleBoundaryTests(unittest.TestCase):
    def test_backend_rejects_reverse_build_dependency_but_allows_worker_interface(self):
        self.assertFalse(boundaries.backend_errors("WorkerProtocol.cpp", "ApplicationServices::Run(options);"))
        for text in ('add_subdirectory(AudioLibraryToolkit.Desktop)', 'COMMAND npm run build',
                     '#include "../ui/client.h"'):
            with self.subTest(text=text):
                self.assertTrue(boundaries.backend_errors("CMakeLists.txt", text))

    def test_client_rejects_private_backend_and_sdk_host_dependency(self):
        self.assertFalse(boundaries.client_errors("sdk.ts", 'import { spawn } from "node:child_process";', sdk=True))
        self.assertTrue(boundaries.client_errors("sdk.ts", 'import { app } from "electron";', sdk=True))
        self.assertTrue(boundaries.client_errors("sdk.ts", 'import "react";', sdk=True))
        self.assertTrue(boundaries.client_errors("app.ts", 'import "../AudioLibraryToolkit/src/app/private";'))

    def test_documentation_links_fail_when_owner_is_missing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guide = root / "guide.md"
            guide.write_text("[API](api.md)\n[Web](https://example.com)\n", encoding="utf-8")
            self.assertEqual(1, len(boundaries.link_errors(guide)))
            (root / "api.md").write_text("# API\n", encoding="utf-8")
            self.assertFalse(boundaries.link_errors(guide))


if __name__ == "__main__":
    unittest.main()
