# Set up the desktop client

Install or build the desktop separately. In Settings, choose a compatible AudioLibraryToolkit executable and, if
needed, its configuration file. The executable can be installed anywhere; backend source is not required.

Choose an output root outside the source library and an initial library. Create a new configuration through Settings
or select an existing one. Creation refuses to overwrite an existing config. FFmpeg/native runtime prerequisites
belong to the toolkit installation; they are not included in the desktop installer.

Choose an operation and its input directory. Conversion/repair require a preview followed by execution confirmation.
For MP3 tag issues, choose resolutions or keep the configured backend policy. Duplicate detection can save a report;
review the report, select specific albums and confirm before quarantining. Refresh catalogs after a move.

If startup fails, verify the executable path and protocol version. If an operation fails, inspect its output/errors
and backend log; do not assume cancellation or failure rolled back completed outputs.

The public [protocol](../../../docs/api/worker-protocol-v1.md) defines capabilities and safety semantics.
