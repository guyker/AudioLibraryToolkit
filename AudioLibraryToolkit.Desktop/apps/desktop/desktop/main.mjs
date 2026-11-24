import { app, BrowserWindow, dialog, ipcMain } from "electron";
import { existsSync } from "node:fs";
import { lstat, readFile, mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const currentDirectory = path.dirname(fileURLToPath(import.meta.url));
const runningOperations = new Map();
let operationStarting = false;
let WorkerClient;

async function loadWorkerClient() {
  const moduleUrl = app.isPackaged
    ? pathToFileURL(path.join(process.resourcesPath, "worker-client", "index.js")).href
    : new URL("../../../packages/worker-client/dist/index.js", import.meta.url).href;
  ({ WorkerClient } = await import(moduleUrl));
}

function settingsFile() {
  return path.join(app.getPath("userData"), "ui-settings.json");
}

async function getSettings() {
  const file = settingsFile();
  if (!existsSync(file)) {
    return { backendPath: "", configPath: "", outputDirectory: "" };
  }
  const parsed = JSON.parse(await readFile(file, "utf8"));
  if (typeof parsed !== "object" || parsed === null ||
      typeof parsed.backendPath !== "string" ||
      typeof parsed.configPath !== "string" ||
      typeof parsed.outputDirectory !== "string") {
    throw new Error("The desktop settings file is invalid. Remove or repair ui-settings.json.");
  }
  return { ...parsed, libraryPath: typeof parsed.libraryPath === "string" ? parsed.libraryPath : "" };
}

async function saveSettings(value) {
  if (typeof value !== "object" || value === null ||
      typeof value.backendPath !== "string" ||
      typeof value.configPath !== "string" ||
      typeof value.outputDirectory !== "string" ||
      typeof value.libraryPath !== "string") {
    throw new TypeError("Settings must contain backendPath, configPath, outputDirectory, and libraryPath strings.");
  }
  const file = settingsFile();
  await mkdir(path.dirname(file), { recursive: true });
  await writeFile(file, JSON.stringify(value, null, 2), "utf8");
  return value;
}

async function readDuplicateReport(reportPath) {
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

function isOperationRequest(value) {
  const operations = ["convert", "scan", "duplicates", "databaseImport", "repair", "initializeConfig"];
  return typeof value === "object" && value !== null &&
    operations.includes(value.operation) &&
    typeof value.parameters === "object" && value.parameters !== null &&
    !Array.isArray(value.parameters) &&
    (value.mode === undefined || value.mode === "preview" || value.mode === "run");
}

function createWindow() {
  const window = new BrowserWindow({
    width: 1440,
    height: 920,
    minWidth: 980,
    minHeight: 680,
    backgroundColor: "#17252a",
    title: "AudioLibraryToolkit",
    webPreferences: {
      preload: path.join(currentDirectory, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });

  if (process.env.VITE_DEV_SERVER_URL) {
    void window.loadURL(process.env.VITE_DEV_SERVER_URL);
  } else {
    void window.loadFile(path.join(currentDirectory, "../build/renderer/index.html"));
  }
}

ipcMain.handle("settings:get", getSettings);
ipcMain.handle("settings:save", (_event, value) => saveSettings(value));
ipcMain.handle("dialog:directory", async () => {
  const result = await dialog.showOpenDialog({ properties: ["openDirectory"] });
  return result.canceled ? null : result.filePaths[0];
});
ipcMain.handle("dialog:file", async (_event, kind) => {
  const filters = kind === "backend"
    ? process.platform === "win32"
      ? [{ name: "AudioLibraryToolkit", extensions: ["exe"] }]
      : [{ name: "AudioLibraryToolkit", extensions: ["*"] }]
    : [{ name: "Configuration", extensions: ["json"] }];
  const result = await dialog.showOpenDialog({ properties: ["openFile"], filters });
  return result.canceled ? null : result.filePaths[0];
});
ipcMain.handle("dialog:save-duplicate-report", async () => {
  const result = await dialog.showSaveDialog({
    title: "Save duplicate report",
    defaultPath: path.join(app.getPath("documents"), "audio-library-duplicates.json"),
    filters: [{ name: "JSON report", extensions: ["json"] }],
  });
  return result.canceled ? null : result.filePath;
});
ipcMain.handle("duplicates:read-report", (_event, reportPath) => readDuplicateReport(reportPath));
ipcMain.handle("operation:start", async (event, request) => {
  if (!isOperationRequest(request) || typeof request.requestId !== "string" ||
      request.requestId.length === 0) {
    throw new TypeError("Invalid operation request.");
  }
  if (operationStarting || runningOperations.size > 0) {
    throw new Error("Another backend operation is already running.");
  }
  operationStarting = true;
  const requestId = request.requestId;
  try {
    const settings = await getSettings();
    if (!settings.backendPath) throw new Error("Choose the AudioLibraryToolkit backend executable in Settings.");

    const controller = new AbortController();
    runningOperations.set(requestId, controller);
    void (async () => {
      try {
        const client = new WorkerClient(settings.backendPath);
        const parameters = {
          ...request.parameters,
          ...(settings.configPath ? { configPath: settings.configPath } : {}),
        };
        const result = await client.run(
          { operation: request.operation, parameters, mode: request.mode },
          {
            requestId,
            signal: controller.signal,
            onEvent: (workerEvent) => {
              if (!event.sender.isDestroyed()) {
                event.sender.send("operation:event", { requestId, event: workerEvent });
              }
            },
          },
        );
        if (!event.sender.isDestroyed()) event.sender.send("operation:result", result);
      } catch (error) {
        if (!event.sender.isDestroyed()) {
          event.sender.send("operation:error", {
            requestId,
            message: error instanceof Error ? error.message : String(error),
            diagnostics: error && typeof error === "object" && "diagnostics" in error
              ? String(error.diagnostics)
              : "",
          });
        }
      } finally {
        runningOperations.delete(requestId);
      }
    })();
    return requestId;
  } finally {
    operationStarting = false;
  }
});
ipcMain.handle("operation:cancel", (_event, requestId) => {
  const controller = runningOperations.get(requestId);
  if (!controller) return false;
  controller.abort();
  return true;
});

app.whenReady()
  .then(async () => {
    await loadWorkerClient();
    createWindow();
    app.on("activate", () => {
      if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
  })
  .catch((error) => {
    dialog.showErrorBox("AudioLibraryToolkit could not start", String(error));
    app.quit();
  });

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") app.quit();
});
