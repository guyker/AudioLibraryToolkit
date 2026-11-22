# Third-party notices

AudioLibraryToolkit uses the following components. Their licences apply independently of the project's licence.
Installed licence texts are in the `licenses` directory next to this notice.

| Component | Licence | Source / licence text |
|---|---|---|
| CLI11 | BSD-3-Clause | `external/CLI11/LICENSE`; installed as `CLI11.txt` |
| spdlog | MIT | `external/spdlog/LICENSE`; installed as `spdlog.txt` |
| fmt (bundled with spdlog) | MIT with optional exception | `external/spdlog/include/spdlog/fmt/bundled/fmt.license.rst`; installed as `fmt.txt` |
| RapidJSON and msinttypes | MIT and BSD-3-Clause | `licenses/RapidJSON.txt` |
| SQLite amalgamation | Public domain | `external/sqlite-amalgamation/sqlite3.c`; [SQLite copyright](https://www.sqlite.org/copyright.html) |
| TagLib | LGPL-2.1 / MPL-1.1 | The CMake-fetched TagLib source includes `COPYING.LGPL` and `COPYING.MPL`; installed as `TagLib-LGPL.txt` and `TagLib-MPL.txt` |
| utfcpp (bundled with TagLib) | Boost Software License 1.0 | TagLib's `3rdparty/utfcpp/LICENSE`; installed as `utfcpp.txt` when present |
| FFmpeg libraries and tools | Depends on the selected build | See the selected SDK or system package. The documented Windows shared GPL SDK is GPL-3.0-or-later; its `LICENSE.txt`, when supplied, is installed as `FFmpeg.txt`. |

An FFmpeg build can include additional third-party libraries with their own notices and source obligations.
Distributors must review the exact FFmpeg build, its component licences and corresponding-source requirements;
this notice alone does not establish distribution compliance. See [FFmpeg's licensing guidance](https://ffmpeg.org/legal.html).
