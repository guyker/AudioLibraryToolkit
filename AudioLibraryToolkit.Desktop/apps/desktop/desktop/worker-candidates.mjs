import path from "node:path";

export function getUnpackagedToolkitCandidate(isPackaged, currentDirectory, platform) {
  if (isPackaged) return null;
  const executableName = platform === "win32" ? "AudioLibraryToolkit.exe" : "AudioLibraryToolkit";
  const buildPreset = platform === "win32" ? "windows-debug" : "linux-debug";
  return path.resolve(currentDirectory, "..", "..", "..", "..", "out", "build", buildPreset,
    "AudioLibraryToolkit", executableName);
}
