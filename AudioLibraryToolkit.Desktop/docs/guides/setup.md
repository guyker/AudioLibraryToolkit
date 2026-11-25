# Set up the desktop client

Install or build the desktop separately. On first run, open Settings and either choose an AudioLibraryToolkit
executable or use **Discover executable**. Discovery checks a short list of standard locations and `PATH`, and
selects only a binary that answers the worker protocol. The executable can be installed anywhere; backend source is
not required.

Choose **Use existing configuration** to edit the backend's config in place, or **Create with backend defaults**
when the resolved file is missing. The backend seeds the editable file from its shipped commented template when
available, without preset library/output paths. Blank Optional configuration file uses the backend's per-user
`config.json`. Creating is explicit and never overwrites an existing file. Backend configuration tabs group library paths, scan,
conversion, duplicate, performance and advanced settings. Save validates through the backend, preserves comments and
unknown keys, and rejects stale edits. Reads never create missing files. FFmpeg/native runtime prerequisites belong
to the toolkit installation; they are not included in the desktop installer.

Operation forms start with the backend's configured source library and output root; those defaults are not sent as
overrides. Catalog JSON inputs for duplicate detection and database import are separate from audio-library paths.
Choose an operation and its input directory. Conversion/repair require a preview followed by execution confirmation.
For MP3 tag issues, choose resolutions or keep the configured backend policy. Duplicate detection can save a report;
review the report, select specific albums and confirm before quarantining. Quarantine copies albums; originals are unchanged. Delete reviewed originals yourself, then refresh catalogs.

The status strip above the workspace shows the run message, progress and the latest output line. Select **View logs**
to open the full operation log over the window (press Escape to close), copy it, or jump to the latest line. Select
**Open in new window** to keep logs in a separate window, for example on a second monitor. Logs do not open by
themselves; after a preview or failure the status message tells you to review them. Both views retain only the latest
200,000 characters of the current or most recent operation, and are cleared when the next operation starts.

If startup fails, verify the executable path and protocol version. If an operation fails, inspect its output/errors
and backend log; do not assume cancellation or failure rolled back completed outputs.

The public [protocol](../../../docs/api/worker-protocol-v1.md) defines capabilities and safety semantics.
