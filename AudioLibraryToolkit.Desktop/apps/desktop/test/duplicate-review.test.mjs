import { strict as assert } from "node:assert";
import { test } from "node:test";
import {
  canSelectAlbum,
  duplicateFindParameters,
  duplicateQuarantineParameters,
  quarantineFolder,
  toggleAlbumSelection,
} from "../renderer/src/duplicateReview.mjs";

const group = { albums: ["D:\\Music\\A", "D:\\Music\\B", "D:\\Music\\C"] };
const [albumA, albumB, albumC] = group.albums;

test("Find is report-only and never carries a quarantine selection", () => {
  const request = duplicateFindParameters({
    catalogDirectory: "C:\\catalog",
    reportPath: "  C:\\out\\duplicates.json  ",
    openFolders: true,
  });
  assert.deepEqual(request, {
    jsonDirectory: "C:\\catalog",
    openFolders: true,
    reportPath: "C:\\out\\duplicates.json",
  });
  assert.equal("duplicateQuarantinePaths" in request, false);
});

test("Find omits a blank report path and only treats true as open-folders", () => {
  assert.deepEqual(
    duplicateFindParameters({ catalogDirectory: "C:\\catalog", reportPath: "   ", openFolders: "yes" }),
    { jsonDirectory: "C:\\catalog", openFolders: false });
  assert.deepEqual(
    duplicateFindParameters({ catalogDirectory: "C:\\catalog" }),
    { jsonDirectory: "C:\\catalog", openFolders: false });
});

test("Quarantine sends only the selected albums, without report or open-folders options", () => {
  const selected = [albumA];
  const request = duplicateQuarantineParameters({
    catalogDirectory: "C:\\catalog",
    outputDirectory: "  C:\\out ",
    configuredOutputDirectory: "C:\\out",
    albumPaths: selected,
  });
  assert.deepEqual(request, { jsonDirectory: "C:\\catalog", duplicateQuarantinePaths: [albumA] });
  assert.notEqual(request.duplicateQuarantinePaths, selected);
});

test("Quarantine overrides the output root only when it differs from the configured one", () => {
  const request = duplicateQuarantineParameters({
    catalogDirectory: "C:\\catalog",
    outputDirectory: "E:\\other",
    configuredOutputDirectory: "C:\\out",
    albumPaths: [albumB],
  });
  assert.equal(request.outputDirectory, "E:\\other");
});

test("the quarantine folder is shown inside the output root using its own separator", () => {
  assert.equal(quarantineFolder("C:\\out"), "C:\\out\\quarantine");
  assert.equal(quarantineFolder("C:\\out\\"), "C:\\out\\quarantine");
  assert.equal(quarantineFolder("/music/out/"), "/music/out/quarantine");
  assert.equal(quarantineFolder("   "), "");
});

test("at least one album in a group stays unmarked", () => {
  assert.equal(canSelectAlbum(group, albumA, [], []), true);
  assert.equal(canSelectAlbum(group, albumC, [albumA], []), true);
  assert.equal(canSelectAlbum(group, albumC, [albumA, albumB], []), false);
  assert.equal(canSelectAlbum(group, albumA, [albumA, albumB], []), true, "a selected album can be unchecked");
});

test("albums already copied to quarantine count as marked and cannot be selected again", () => {
  assert.equal(canSelectAlbum(group, albumA, [], [albumA]), false);
  assert.equal(canSelectAlbum(group, albumB, [], [albumA]), true);
  assert.equal(canSelectAlbum(group, albumC, [albumB], [albumA]), false);
});

test("toggling checks, unchecks and refuses to mark the last remaining album", () => {
  assert.deepEqual(toggleAlbumSelection([], group, albumA, true, []), [albumA]);
  assert.deepEqual(toggleAlbumSelection([albumA], group, albumA, true, []), [albumA]);
  assert.deepEqual(toggleAlbumSelection([albumA, albumB], group, albumA, false, []), [albumB]);
  assert.deepEqual(toggleAlbumSelection([albumA, albumB], group, albumC, true, []), [albumA, albumB]);
  assert.deepEqual(toggleAlbumSelection([albumB], group, albumC, true, [albumA]), [albumB]);
});
