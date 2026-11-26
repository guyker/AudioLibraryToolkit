import { lstat, readFile } from "node:fs/promises";

export async function readDuplicateReport(reportPath) {
  if (typeof reportPath !== "string" || !reportPath.trim()) {
    throw new TypeError("A duplicate report path is required.");
  }
  const fileInfo = await lstat(reportPath);
  if (!fileInfo.isFile() || fileInfo.isSymbolicLink() || fileInfo.size > 20 * 1024 * 1024) {
    throw new Error("The duplicate report must be a regular file no larger than 20 MiB.");
  }
  const parsed = JSON.parse(await readFile(reportPath, "utf8"));
  if (typeof parsed !== "object" || parsed === null || parsed.schemaVersion !== 1 ||
      typeof parsed.status !== "string" || typeof parsed.statistics !== "object" ||
      parsed.statistics === null || !Array.isArray(parsed.groups)) {
    throw new Error("The selected file is not a supported duplicate report.");
  }
  const groups = parsed.groups.map((group) => {
    if (typeof group !== "object" || group === null || !Array.isArray(group.albums) ||
        !group.albums.every((album) => typeof album === "string") ||
        !Array.isArray(group.pairs)) {
      throw new Error("The duplicate report contains an invalid group.");
    }
    const pairs = group.pairs.map((pair) => {
      if (typeof pair !== "object" || pair === null ||
          typeof pair.album1 !== "string" || typeof pair.album2 !== "string" ||
          typeof pair.confidence !== "string" || !Array.isArray(pair.evidence) ||
          !pair.evidence.every((item) => typeof item === "string")) {
        throw new Error("The duplicate report contains an invalid match.");
      }
      return {
        album1: pair.album1,
        album2: pair.album2,
        confidence: pair.confidence,
        evidence: pair.evidence,
      };
    });
    return { albums: group.albums, pairs };
  });
  const statistics = {};
  for (const [key, value] of Object.entries(parsed.statistics)) {
    if (typeof value !== "number" || !Number.isSafeInteger(value) || value < 0) {
      throw new Error("The duplicate report contains invalid statistics.");
    }
    statistics[key] = value;
  }
  return { status: parsed.status, statistics, groups };
}
