import { randomUUID } from "node:crypto";
import path from "node:path";

export function getOperationPaths(libraryPath, outputDirectory) {
  if (typeof libraryPath !== "string" || typeof outputDirectory !== "string") {
    throw new TypeError("Library and output paths must be strings.");
  }
  const output = outputDirectory.trim();
  if (!output) return { catalogDirectory: "", reportPath: "" };
  const root = path.resolve(output);
  const library = libraryPath.trim();
  const name = library ? path.basename(path.resolve(library)) : "";
  return {
    catalogDirectory: name ? path.join(root, "catalog-json", name) : "",
    reportPath: path.join(root, `duplicates-${randomUUID()}.json`),
  };
}
