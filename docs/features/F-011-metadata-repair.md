# F-011: Repair FLAC/MP3 metadata and validate stream integrity

Status: active

## Behaviour
The `repair` command and menu option 7 inspect `.flac` and `.mp3` files against a catalog of metadata rules (see
DESIGN → Metadata repair). Fixable defects include redundant ID3/APE tags, duplicate or empty tags/frames,
fully redundant extra FLAC comment blocks, wrong picture MIME types or FLAC picture dimensions/depth, invalid FLAC seek tables and ID3v2.3 frames using
v2.4-only text encodings. Lyrics3 tags are reported, not removed. Tags are removed only when empty, exact duplicates,
or fully redundant; conflicting values
(including conflicting or semicolon-joined CDTOC), invalid comments, missing FLAC MD5, MP3 stream garbage,
unsupported ID3 layouts and unreadable files are reported for manual review. `--rules <id,...>` limits the CLI to selected rules.
MP3 ID3v1 tags are kept by default for older players; remove redundant ones only with `--remove-id3v1`,
an explicit `--rules mp3.redundant-id3v1` selection, or the menu's per-run opt-in. Existing FLAC padding is
preserved and metadata shrinkage is absorbed into padding when possible.

With default settings repair writes only to a separate destination tree; source files are read-only and the inferred
audio bytes are copied unchanged, never decoded or re-encoded. Repaired files are written to
`<output-root>/repaired-metadata/<library-name>` (`--output-dir`, else `OutDirectory` for the CLI); only folders with a repair are
copied, retaining all other files unchanged. In menu option 7, repair outputs of different active libraries may not
overlap each other or another library's source; the run fails before installing anything. Existing nested output
symlinks/junctions that redirect destinations are rejected and checked again before staging and installation. The
source root and every discovered source entry must also be free of symlinks and reparse points, including
regular-file links; a link fails preparation rather than being followed or copied.
`repair-report.json` records findings, risk labels, separate unchanged/fix risk descriptions, file installation status,
and whether audio or metadata changed. It marks rules as planned until successfully installed, then as applied.
Open the printed `repair-summary.html` path to filter by issue, album folder, track or change. "Issues by severity"
groups findings by issue, highest severity first, then by album folder and track; each album shows whether all or only
some of its tracks have the issue, and each track links its other issues. "Albums by severity" lists only folders with
findings, highest severity first, with affected/total tracks, the issues present and tracks having more than one issue. Each issue states once
the risk of leaving it unchanged and of applying its fix. The report distinguishes verified installed repairs from failures, repairs not
attempted and findings left for review; audio byte verification does not imply equal player compatibility.
Before `RUN`, option 7 shows only finding statistics in the console and saves a searchable HTML preview per library
outside the source libraries. It includes every finding, with proposed edits, reasons and separate unchanged/fix
risks. Cancellation
keeps the preview without creating repair output. Existing
destinations are rejected unless `--overwrite` is specified. The CLI returns non-zero for manual-review findings or
operation failures.
Menu and CLI previews summarize existing destination conflicts without printing individual paths. Full file and
directory conflict paths are reserved for error-level session logging (F-010); sources and existing outputs are
unchanged when these conflicts block repair.
`--dry-run` prints the full JSON findings and planned actions without writing repair output; normal session logging
still applies. Inspection and staging use four workers by default; CLI `--jobs <1-16>` changes the limit.
Before the preview, progress identifies the source folder and the discovery, inspection and planning phases.
Inspection shows completed/total files and a percentage; discovery/planning show counts without an invented total.
During execution, progress shows verified files installed out of all planned repairs and unchanged copies.
Each phase shows elapsed time and remains visibly active during long files. Progress is per library and per phase;
phases with a known total show a rough ETA. Interactive updates reuse one line; redirected output uses periodic snapshots instead
of terminal control sequences. Phase boundaries are retained in the session log in both modes.

## Entry points
- Enter `CATEGORIES` before the menu overwrite/ID3v1 choices, or pass `--metadata-mode`, `--integrity-mode`,
  `--container-mode`, `--decode-mode` to `repair`. Config `RepairCategories` and worker v1 `repairCategories` use
  the same members. Metadata and integrity support `off/detect/fix`; container and decode support `off/detect`.
  Built-ins are `fix/off/off/off`. Rule filters narrow modes; they do not enable a disabled category.
- `repair <input> [--output-dir <directory>] [--overwrite] [--rules <ids>] [--remove-id3v1] [--dry-run] [--jobs <1-16>]` → `AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp#RunWithArgumentsImpl`
  → `AudioLibraryToolkit/src/repair/AudioRepair.cpp#AudioRepair::Prepare`, `AudioLibraryToolkit/src/repair/AudioRepair.cpp#AudioRepair::Execute`
- Menu option 7 → `AudioLibraryToolkit/src/app/AudioLibraryToolkitMain.cpp#RepairConfiguredLibraries`

## Design and knowledge
- DESIGN: `AudioLibraryToolkit/DESIGN.md` → Metadata repair; rules in `AudioLibraryToolkit/src/repair/RepairRules.cpp#RepairRules::Analyze`
- Knowledge: K-065 operation-based output layout; K-080 explains the original PCM witnesses required for a stream edit.

## Acceptance checks
- [ ] Identical duplicate CDTOC comments are reduced to one in the output; source bytes remain unchanged.
- [ ] Menu and CLI use `repaired-metadata/<library-name>`; previous `FIXED` output remains untouched.
- [ ] Fully redundant FLAC leading/trailing tags, valid empty comments, picture MIME/width/height/depth and invalid seek table are fixed; audio bytes unchanged; differing tag values are retained.
- [ ] Invalid FLAC comments are retained for review; a redundant comment block with a different vendor string is not removed.
- [ ] MP3 duplicate/empty frames, v2.3 text, custom-text, comment and lyrics encodings, and APIC MIME are fixed; audio bytes unchanged.
- [ ] Conflicting ID3v1 genres and artwork with a different picture role/description are retained; artwork is redundant only when type, description and bytes match.
- [ ] Combined ID3 text normalization and duplicate-frame removal do not create new duplicates; tag size and chapter offsets remain unchanged, or edits that do not fit are deferred for review.
- [ ] MP3 ID3v1 stays by default and is removed only when redundant and explicitly selected.
- [ ] FLAC padding remains available; metadata shrinkage preserves the audio offset where padding can absorb it.
- [ ] Dry-run prints every finding without creating repair output, and parallel and single-worker runs agree.
- [ ] Conflicting, joined CDTOC, non-redundant tags and unreadable files are reported without changing source files.
- [ ] Re-running repair on repaired output finds nothing to repair; unknown `--rules` ids are rejected.
- [ ] Only affected folders are copied and companion files remain byte-identical.
- [ ] Menu option 7 requires `RUN` and reports the output and completed repair count.
- [ ] A preview with more than 25 findings reports all of them in HTML without printing individual findings to the
      console. Cancelling leaves sources and `repaired-metadata` untouched, and an unsafe temporary location blocks repair.
- [ ] The HTML summary groups track findings by folder, shows compatibility cautions and never claims a failed or
      unattempted repair was verified and installed.
- [ ] Preview and post-repair reports show the same findings by issue (ordered by severity, then album and track) and by album (only albums with findings), mark whole-album versus partial findings and tracks with several issues, and state separate risks for leaving or fixing each issue; JSON distinguishes planned rules from successfully applied rules.
- [ ] Any source file/directory symlink or reparse point fails preparation; nested output links are rejected before writes.
- [ ] Existing output is rejected by default; `--overwrite` allows eligible destinations to be replaced.
- [ ] A retained `.repair-in-progress` marker blocks a run even with `--overwrite`, names the marker path, and leaves
      the marker, output and sources unchanged.
- [ ] A clean library publishes JSON (`no-repairs-needed`) and HTML reports, copies no audio, and removes the
      in-progress marker. Windows writer-close timestamp finalization does not cause a false publication failure;
      replaced temporaries and modified content still fail verification.
- [ ] Large destination conflict sets show only console counts/output locations and guidance; all conflict
      paths are retained in the session log when error-level file logging is enabled.
- [ ] Preparation displays progress before confirmation without writing repair output; cancellation keeps sources unchanged.
- [ ] Installation progress advances only after verification/installation and stops short of 100% on failure.
- [ ] A generated 24-bit/96 kHz FLAC with 4,186 appended zero bytes is clean in default metadata mode,
      reported with its exact byte interval in integrity detect mode, and restored byte-for-byte in integrity fix mode.
      The source remains unchanged; a missing or contradictory PCM witness never authorizes a failing-source trim.
- [ ] STREAMINFO and seek corrections retain the complete compressed frames; metadata detect mode does not silently
      apply a tag change during an integrity repair. Only a strictly verified installation lists applied rules.
- [ ] MP3 stream anomalies and wrong containers never authorize an audio edit. A structural-only run has no full-decode
      result; explicitly requesting decode reports the whole-file result separately from installation.

## Opt-in integrity behavior

Structural inspection checks actual native FLAC frame boundaries, CRCs, continuity, size/sample declarations and
seek targets. A proven post-frame fill region may be removed; unrecognized trailers, truncated frames and gaps
are not cut. Eligible FLAC field edits correct unambiguous declarations, fill a previously unknown MD5 or correct
a seek offset/length for an existing real sample target. Nonzero MD5 mismatches are never restamped. MP3 frame
boundaries, counts, VBR headers/tables, CRCs, embedded tags and parameter changes are diagnostic only.
Container inspection distinguishes mismatches and empty/no-audio inputs under supported extensions; no renaming,
demuxing or discovery of other extensions. `decode=detect` checks whole-file decoding independently.

Each integrity edit requires an exact edit map, comparison of every unchanged byte, and a strict staged decode
with matching PCM and sample count. Without an original FLAC MD5 both whole files must decode cleanly and match.
The only exception for a strict source failure is independently proven FLAC suffix fill with original nonzero MD5
and declared count matching the complete retained program. Default JSON stays schema 2. Non-default runs use
[schema 3](../schemas/repair-report-v3.schema.json), separating detections, pending/refused proofs and installed changes.
Both HTML views show the selected categories. Detect-only is not an edit; no structural finding is not a PCM certificate.

## Known limitations
- Only FLAC and MP3 are supported; only the first leading ID3v2 tag of an MP3 is rewritten.
- Different values are never reconciled automatically. MP3 writes remain metadata-only; no VBR header insertion,
  CRC regeneration, re-encoding, sample insertion or deletion. Unknown or ambiguous data stays for review.
- Metadata-only mode intentionally does not parse FLAC frames or certify the stream. Enable integrity explicitly.
- Output staging uses held directory handles and no-follow file operations where supported; output paths are revalidated
  during preparation, staging, installation and reporting. A separate same-permission process deliberately moving
  directories between checks can still race ordinary filesystem operations.
