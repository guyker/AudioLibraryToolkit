interface AlbumGroup {
  readonly albums: readonly string[];
}

export function duplicateFindParameters(input: {
  catalogDirectory: string;
  reportPath?: unknown;
  openFolders?: unknown;
}): { jsonDirectory: string; openFolders: boolean; reportPath?: string };

export function duplicateQuarantineParameters(input: {
  catalogDirectory: string;
  outputDirectory: string;
  configuredOutputDirectory: string;
  albumPaths: readonly string[];
}): { jsonDirectory: string; outputDirectory?: string; duplicateQuarantinePaths: string[] };

export function quarantineFolder(outputDirectory: string): string;

export function canSelectAlbum(
  group: AlbumGroup,
  album: string,
  selected: readonly string[],
  quarantined: readonly string[],
): boolean;

export function toggleAlbumSelection(
  current: readonly string[],
  group: AlbumGroup,
  album: string,
  checked: boolean,
  quarantined: readonly string[],
): string[];
