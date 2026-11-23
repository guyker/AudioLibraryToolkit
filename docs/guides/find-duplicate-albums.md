# Find duplicate music albums

AudioLibraryToolkit can scan a music library and compare albums that may be duplicates. It groups related copies and
shows the evidence, so you can review differences without automatically deleting files.

## 1. Build and configure

[Build and install](../../README.md#build) the application. On the first operational run in an interactive terminal,
setup asks for an output directory and an existing music library; see
[configuration and first run](../../README.md#configuration-and-first-run) for details.

The examples below use Windows paths. Replace them with your own paths (or Linux paths), and keep output outside the
source music library.

## 2. Scan your music library

```text
AudioLibraryToolkit scan "D:\Music" --output-dir "E:\Catalog"
```

The scan writes JSON batches and a manifest under `E:\Catalog\catalog-json\Music`. FLAC and MP3 are scanned by default. The default
`catalog` tier reads metadata without decoding audio; add `--tier integrity` if you also want full-decode integrity
checks. A successful rescan replaces the previously generated scan batches for that library.

## 3. Compare albums

```text
AudioLibraryToolkit duplicates "E:\Catalog\catalog-json\Music"
```

The command is noninteractive and does not change the music library. It identifies candidates using normalized album
names, MusicBrainz release IDs or stored FLAC MD5s, then compares tracks and reports match evidence and differences.
Use `--open` only if you want matching folders opened in the file manager.

To save a JSON report, supply a **new** file path outside the source library; existing reports are not overwritten:

```text
AudioLibraryToolkit duplicates "E:\Catalog\catalog-json\Music" --report "E:\duplicates-report.json"
```

## 4. Review the results

Inspect the tracks, tags, artwork and companion files before deciding what to keep. Confidence labels describe
rule-based matches, not probabilities; an exact FLAC MD5 match is based on stored scan data and does not prove that
the files are byte-identical. Higher bitrate or resolution alone does not establish better source quality. Rescan if
files have changed since the catalog was created.

The `duplicates` command does not move or delete albums. In the interactive menu, option 3 can move a selected album
folder to `OutDirectory/quarantine` only after a separate `MOVE` confirmation; the folder is copied and verified before
originals are removed. A cleanup failure can leave partial originals beside a complete verified copy. Rescan afterward.

See the [duplicate feature reference](../features/F-005-duplicate-albums.md) for matching rules, options and limitations.
