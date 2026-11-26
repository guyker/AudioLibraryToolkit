// Keys whose values are edited as JSON text instead of a scalar control.
export const complexConfigurationKeys = new Set([
  "MediaDirectoryList", "CatalogExtensions", "FLACSettings", "MetadataSidecars",
]);

// Every loaded, created or saved snapshot must go through this one function so the draft and the JSON
// text editors always describe the same file. Initialising them separately left the text empty after
// "Create with backend defaults" and turned a later save into a patch with null for the complex keys.
export function configurationEditorState(configuration) {
  const text = {};
  for (const key of complexConfigurationKeys) {
    text[key] = JSON.stringify(configuration[key] ?? null, null, 2);
  }
  return { draft: configuration, text };
}

// Only keys that differ from the snapshot are sent, so the backend keeps everything else byte for byte.
// Invalid JSON in a text editor throws and nothing is sent.
export function buildConfigurationPatch(snapshotConfiguration, draft, text) {
  const patch = {};
  for (const key of Object.keys(draft)) {
    const value = complexConfigurationKeys.has(key) ? JSON.parse(text[key] ?? "null") : draft[key];
    if (JSON.stringify(value) !== JSON.stringify(snapshotConfiguration[key])) {
      patch[key] = value;
    }
  }
  return patch;
}
