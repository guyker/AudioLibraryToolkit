import { stat } from "node:fs/promises";
import path from "node:path";

// The renderer may open only album folders that the most recently read duplicate report listed, so a
// compromised or buggy page cannot use "open folder" as a general file launcher.
export function createAlbumFolderGate(statPath = stat) {
  let reportedAlbums = new Set();
  return {
    remember(report) {
      reportedAlbums = new Set(report.groups.flatMap((group) => group.albums));
    },
    async resolve(albumPath) {
      if (typeof albumPath !== "string" || !path.isAbsolute(albumPath) || !reportedAlbums.has(albumPath)) {
        throw new Error("Only album folders from the current duplicate report can be opened.");
      }
      let info;
      try {
        info = await statPath(albumPath);
      } catch {
        throw new Error(`The album folder is not available: ${albumPath}`);
      }
      if (!info.isDirectory()) throw new Error(`The album path is not a folder: ${albumPath}`);
      return albumPath;
    },
  };
}
