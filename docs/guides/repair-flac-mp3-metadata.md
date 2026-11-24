# Repair FLAC and MP3 metadata

AudioLibraryToolkit inspects FLAC and MP3 files for recognized metadata defects and can write repaired copies to a
separate output tree. Repair does not modify source files or re-encode their audio.

## 1. Build and configure

[Build and install](../../AudioLibraryToolkit/README.md#build) the application. On the first operational run in an interactive terminal,
setup asks for an output directory and an existing music library; see
[configuration and first run](../../AudioLibraryToolkit/README.md#configuration-and-first-run) for details.

The examples below use Windows paths. Replace them with your own paths (or Linux paths), and keep output outside the
source music library.

## 2. Preview metadata repairs

```text
AudioLibraryToolkit repair "D:\Music" --output-dir "E:\Repaired" --dry-run
```

The dry run prints JSON findings and planned actions without writing repair output. Normal session logs still apply.
Findings can include redundant or empty tags, invalid FLAC seek tables, picture metadata problems and ID3 encoding
issues. Conflicting values, unsupported layouts and other uncertain findings are left for manual review.

## 3. Write repaired copies

After reviewing the findings, run the command without `--dry-run`:

```text
AudioLibraryToolkit repair "D:\Music" --output-dir "E:\Repaired"
```

Repaired copies go under `E:\Repaired\repaired-metadata\Music`. Only affected folders are copied; companion files in those folders
are retained. Existing destinations are rejected unless you explicitly enable `--overwrite`. The command reports
manual-review findings and failures with a nonzero exit code.

Review `repair-report.json` and the printed `repair-summary.html` path for installed changes and unresolved findings.
Audio bytes are verified unchanged, but that does not guarantee every player's metadata compatibility. Redundant
MP3 ID3v1 tags are kept by default for older players.

For an interactive HTML preview before repair, run `AudioLibraryToolkit` without arguments and choose menu option 7;
the preview requires a separate `RUN` confirmation before repair output is written.

See the [repair feature reference](../features/F-011-metadata-repair.md) for supported rules and limitations.
