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
| FFmpeg libraries and tools | GPL-3.0-or-later for the pinned GPL-shared build | Platform archives and SHA-256 are in `cmake/ffmpeg-lock.json`; the SDK's `LICENSE.txt` is installed as `FFmpeg.txt`. |
| MSVC runtime (Windows Release) | Microsoft redistribution terms | App-local redistributable runtime selected by CMake `InstallRequiredSystemLibraries`; review the applicable Visual Studio licence and redistributable list. |
| GCC runtime (Linux) | GPL with GCC Runtime Library Exception where applicable | Installation copies required non-system runtime libraries and available `gcc-*-base` copyright notices. Review the exact packaged dependency inventory. |

An FFmpeg build can include additional third-party libraries with their own notices and source obligations.
Distributors must review the exact FFmpeg build, its component licences and corresponding-source requirements;
this notice alone does not establish distribution compliance. See [FFmpeg's licensing guidance](https://ffmpeg.org/legal.html).

The pinned SDK configuration enables GPL/version-3 components and shared linkage. Shared linkage does not make this
build LGPL. The SDK licence text and headers are not a complete Corresponding Source bundle. Exact dependency
sources, patches and build materials have not all been independently established; the engineering candidate is
therefore **not cleared for redistribution**. `cmake/ffmpeg-redistribution.json` records this pending gate, and
`tools/package_application.py` refuses normal release packaging until matching source materials are reviewed and
available. Development-only artifacts are for local verification, not evidence of clearance.

Distributors must preserve applicable notices and provide the exact application's and covered dependencies'
Corresponding Source, including required build/install scripts. A separate matching source companion download beside
the binary release, with clear directions and no additional source fee, is the preferred delivery mechanism (see
GPLv3 section 6(d) in `LICENSE`); it need not be embedded in every runtime archive. A generic FFmpeg homepage or an
unrelated source snapshot does not establish compliance. Review System Library exclusions, other component licences
and relevant distribution/patent obligations for the actual release. Do not distribute a nonfree build.
