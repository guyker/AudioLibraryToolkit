# Set up the desktop client

Install or build the desktop separately. On first run, open Settings and either choose an AudioLibraryToolkit
executable or use **Discover executable**. Discovery checks standard executable locations and `PATH`; only an
unpackaged development build also probes the repository's `out\build` candidate. It selects only a binary that answers
the worker protocol. The executable can be installed anywhere; backend source is not required.

Choose **Use existing configuration** to edit the backend's config in place, or **Create with backend defaults**
when the resolved file is missing. The backend seeds the editable file from its shipped commented template when
available, without preset library/output paths. Blank Optional configuration file uses the backend's per-user
`config.json`. Reading or validating a missing file creates nothing. Saving edits from a missing-file snapshot creates
a config from built-in defaults and has no prior file to back up; explicit **Create with backend defaults** uses the
commented template when available and refuses to overwrite an existing file. Replacing an existing config keeps a
`.bak` recovery copy. Backend configuration tabs group library paths, scan,
conversion, duplicate, performance and advanced settings. Save validates through the backend, writes only the settings
you changed, preserves comments and unknown keys, and rejects stale edits. Nested settings objects are merged; an edited
list, such as the media directories, is replaced as a whole, so comments inside that list are lost. Reads never create
missing files. FFmpeg/native runtime prerequisites belong
to the toolkit installation; they are not included in the desktop installer.

Operation forms prefill and send the selected input path, including a configured default. Catalog JSON inputs for
duplicate detection and database import are separate from audio-library paths. An unchanged output root is omitted
from ordinary requests; a changed output override is sent, and the separate Quarantine action sends its required root.
Choose an operation and its input directory. Conversion/repair require a preview followed by execution confirmation.
For MP3 tag issues, choose resolutions or keep the configured backend policy. **Find duplicate albums** only writes a
report (set an output root or choose a report file, otherwise matches appear only in the logs); it never copies or
deletes albums. In the report, **Open folder** shows one matching album in the file manager and **Open all folders**
every album of a group, so you can compare editions and delete the unwanted ones yourself. Alternatively select specific
albums (at least one per group stays unmarked) and confirm **Quarantine**, which copies and verifies them under
`<output root>\quarantine`. Originals are unchanged, the app never deletes albums, and the report stays visible with
copied albums marked. After deleting originals, refresh the catalogs.

The status strip above the workspace shows the run message, progress and the latest output line. Select **View logs**
to open the full operation log over the window (press Escape to close), copy it, or jump to the latest line. Select
**Open in new window** to keep logs in a separate window, for example on a second monitor. Logs do not open by
themselves; after a preview or failure the status message tells you to review them. Both views retain only the latest
200,000 characters of the current or most recent operation, and are cleared when the next operation starts.

If startup fails, verify the executable path and protocol version. If an operation fails, inspect its output/errors
and backend log; do not assume cancellation or failure rolled back completed outputs.

The public [worker protocol v1](https://github.com/guyker/AudioLibraryToolkit/blob/main/docs/api/worker-protocol-v1.md)
defines capabilities and safety semantics.
