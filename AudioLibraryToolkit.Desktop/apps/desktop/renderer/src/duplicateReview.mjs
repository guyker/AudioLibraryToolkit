// Find is report-only. Quarantine copies albums, so it is a separate request built only from an explicit,
// confirmed selection. Building both from one function let a plain Find re-send the albums checked in the
// previous report as a quarantine request.
export function duplicateFindParameters({ catalogDirectory, reportPath, openFolders }) {
  const report = typeof reportPath === "string" ? reportPath.trim() : "";
  return {
    jsonDirectory: catalogDirectory,
    openFolders: openFolders === true,
    ...(report ? { reportPath: report } : {}),
  };
}

export function duplicateQuarantineParameters({
  catalogDirectory, outputDirectory, configuredOutputDirectory, albumPaths,
}) {
  const output = outputDirectory.trim();
  return {
    jsonDirectory: catalogDirectory,
    ...(output !== configuredOutputDirectory.trim() ? { outputDirectory: output } : {}),
    duplicateQuarantinePaths: [...albumPaths],
  };
}

// Display only: the backend decides the real location, which is <output root>/quarantine.
export function quarantineFolder(outputDirectory) {
  const root = outputDirectory.trim();
  if (!root) return "";
  return `${root.replace(/[\\/]+$/, "")}${root.includes("\\") ? "\\" : "/"}quarantine`;
}

// Albums already copied to quarantine still count against the keep-at-least-one rule of their group.
function markedAlbumCount(group, selected, quarantined) {
  return group.albums.filter((album) => selected.includes(album) || quarantined.includes(album)).length;
}

export function canSelectAlbum(group, album, selected, quarantined) {
  if (quarantined.includes(album)) return false;
  return selected.includes(album) || markedAlbumCount(group, selected, quarantined) < group.albums.length - 1;
}

export function toggleAlbumSelection(current, group, album, checked, quarantined) {
  if (!checked) return current.filter((path) => path !== album);
  if (current.includes(album) || !canSelectAlbum(group, album, current, quarantined)) return current;
  return [...current, album];
}
