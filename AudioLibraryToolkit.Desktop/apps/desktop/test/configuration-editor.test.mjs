import { strict as assert } from "node:assert";
import { test } from "node:test";
import {
  buildConfigurationPatch,
  complexConfigurationKeys,
  configurationEditorState,
} from "../renderer/src/configurationEditor.mjs";

function backendDefaults() {
  return {
    OutDirectory: "",
    LogLevel: 2,
    MediaDirectoryList: [{ mediaPath: "D:\\Music", isActive: true }],
    CatalogExtensions: [".flac", ".mp3"],
    FLACSettings: { SampleRate: 44100 },
    MetadataSidecars: { Format: "json" },
  };
}

test("a fresh snapshot fills every JSON editor, so an untouched state sends nothing", () => {
  const configuration = backendDefaults();
  const { draft, text } = configurationEditorState(configuration);
  assert.deepEqual(Object.keys(text).sort(), [...complexConfigurationKeys].sort());
  for (const key of complexConfigurationKeys) assert.deepEqual(JSON.parse(text[key]), configuration[key]);
  assert.deepEqual(buildConfigurationPatch(configuration, draft, text), {});
});

test("changing one scalar after Create sends only that key and never nulls the JSON editors", () => {
  const configuration = backendDefaults();
  const { draft, text } = configurationEditorState(configuration);
  assert.deepEqual(buildConfigurationPatch(configuration, { ...draft, LogLevel: 3 }, text), { LogLevel: 3 });
});

test("only an edited JSON editor is sent; reformatting alone is not an edit", () => {
  const configuration = backendDefaults();
  const { draft, text } = configurationEditorState(configuration);
  assert.deepEqual(
    buildConfigurationPatch(configuration, draft, { ...text, CatalogExtensions: "[\".flac\"]" }),
    { CatalogExtensions: [".flac"] },
  );
  assert.deepEqual(
    buildConfigurationPatch(configuration, draft, {
      ...text, MediaDirectoryList: JSON.stringify(configuration.MediaDirectoryList),
    }),
    {},
  );
});

test("each snapshot rebuilds the JSON text, so another file's values never carry over", () => {
  const previous = configurationEditorState({ ...backendDefaults(), CatalogExtensions: [".ogg"] });
  const created = backendDefaults();
  const next = configurationEditorState(created);
  assert.notEqual(next.text.CatalogExtensions, previous.text.CatalogExtensions);
  assert.deepEqual(buildConfigurationPatch(created, next.draft, next.text), {});
});

test("invalid JSON in an editor throws, so nothing is sent", () => {
  const configuration = backendDefaults();
  const { draft, text } = configurationEditorState(configuration);
  assert.throws(
    () => buildConfigurationPatch(configuration, draft, { ...text, MediaDirectoryList: "[" }),
    SyntaxError,
  );
});
