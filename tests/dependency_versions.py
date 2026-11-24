import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


CMAKE = sys.argv.pop(1)
SCRIPT = sys.argv.pop(1)


class DependencyVersionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.taglib = self.root / "taglib"
        self.taglib_build = self.root / "taglib-build"
        self.ffmpeg = self.root / "ffmpeg"
        self.zlib = self.root / "zlib"

        self.write(self.taglib / "taglib/toolkit/taglib.h",
                   "#define TAGLIB_MAJOR_VERSION 2\n#define TAGLIB_MINOR_VERSION 3\n"
                   "#define TAGLIB_PATCH_VERSION 2\n")
        self.write(self.source / "external/CLI11/include/CLI/Version.hpp",
                   '#define CLI11_VERSION "2.7.2"\n')
        self.write(self.source / "external/spdlog/include/spdlog/version.h",
                   "#define SPDLOG_VER_MAJOR 1\n#define SPDLOG_VER_MINOR 15\n#define SPDLOG_VER_PATCH 3\n")
        self.write(self.source / "external/spdlog/include/spdlog/fmt/bundled/base.h",
                   "#define FMT_VERSION 110200\n")
        self.write(self.source / "external/sqlite-amalgamation/sqlite3.h",
                   '#define SQLITE_VERSION "3.50.0"\n')
        self.write(self.source / "AudioLibraryToolkit/include/rapidjson/rapidjson.h",
                   "#define RAPIDJSON_MAJOR_VERSION 1\n#define RAPIDJSON_MINOR_VERSION 1\n"
                   "#define RAPIDJSON_PATCH_VERSION 0\n")
        self.write(self.ffmpeg / "libavutil/ffversion.h", '#define FFMPEG_VERSION "sdk-version"\n')
        for component in ("avformat", "avcodec", "avfilter", "avutil", "swresample"):
            prefix = f"LIB{component.upper()}_VERSION"
            self.write(self.ffmpeg / f"lib{component}/version.h",
                       f"#define {prefix}_MINOR 12\n#define {prefix}_MICRO 100\n")
            self.write(self.ffmpeg / f"lib{component}/version_major.h",
                       f"#define {prefix}_MAJOR 7\n")
        self.write(self.taglib_build / "3rdparty/utfcpp/utf8cppConfigVersion.cmake",
                   'set(PACKAGE_VERSION "4.2.0")\n')
        self.write(self.taglib_build / "config.h", "#define HAVE_ZLIB 1\n")
        self.write(self.zlib / "zlib.h", '#define ZLIB_VERSION "1.3.2"\n')

    @staticmethod
    def write(path, content):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def report(self, *extra):
        result = subprocess.run(
            [CMAKE, f"-DREPORT_SOURCE_DIR={self.source}",
             f"-DREPORT_TAGLIB_SOURCE_DIR={self.taglib}",
             f"-DREPORT_TAGLIB_BINARY_DIR={self.taglib_build}",
             f"-DREPORT_FFMPEG_INCLUDE_DIR={self.ffmpeg}",
             f"-DREPORT_ZLIB_INCLUDE_DIR={self.zlib}",
             *extra, "-P", SCRIPT],
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout + result.stderr

    def test_reports_versions_from_selected_sources(self):
        output = self.report()
        for expected in (
            "TagLib: 2.3.2", "CLI11 (submodule): 2.7.2", "spdlog (submodule): 1.15.3",
            "fmt (bundled with spdlog): 11.2.0", "SQLite: 3.50.0", "RapidJSON: 1.1.0",
            "FFmpeg headers: sdk-version", "FFmpeg libavformat headers: 7.12.100",
            "FFmpeg libswresample headers: 7.12.100",
            "utfcpp (bundled with TagLib): 4.2.0", "ZLIB (TagLib): 1.3.2",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, output)

    def test_reports_selected_system_utfcpp_and_disabled_zlib(self):
        self.write(self.taglib_build / "config.h", "/* #undef HAVE_ZLIB */\n")
        output = self.report("-DREPORT_UTFCPP_FOUND=TRUE", "-DREPORT_UTFCPP_VERSION=5.1.0")
        self.assertIn("utfcpp (system): 5.1.0", output)
        self.assertNotIn("utfcpp (bundled with TagLib)", output)
        self.assertIn("ZLIB (TagLib): not enabled", output)

    def test_missing_version_is_reported_as_unknown(self):
        (self.taglib / "taglib/toolkit/taglib.h").unlink()
        self.assertIn("TagLib: unknown (version header unavailable)", self.report())


if __name__ == "__main__":
    unittest.main()
