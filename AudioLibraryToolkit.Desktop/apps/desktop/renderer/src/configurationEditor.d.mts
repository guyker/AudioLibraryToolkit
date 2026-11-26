export const complexConfigurationKeys: ReadonlySet<string>;

export function configurationEditorState(configuration: Record<string, unknown>): {
  draft: Record<string, unknown>;
  text: Record<string, string>;
};

export function buildConfigurationPatch(
  snapshotConfiguration: Record<string, unknown>,
  draft: Record<string, unknown>,
  text: Record<string, string>,
): Record<string, unknown>;
