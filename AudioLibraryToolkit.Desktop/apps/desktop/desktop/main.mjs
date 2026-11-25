import { app, BrowserWindow, dialog, ipcMain } from "electron";
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { lstat, readFile, mkdir, writeFile, copyFile } from "node:fs/promises";
import path from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath, pathToFileURL } from "node:url";
import { getOperationPaths } from "./operation-paths.mjs";

const currentDirectory = path.dirname(fileURLToPath(import.meta.url));
app.setName("AudioLibraryToolkit Desktop");
const runningOperations = new Map();
let operationStarting = false;
let WorkerClient;
const logLimit = 200_000;
let operationLog = "";
let operationLogRevision = 0;
let mainWindow = null;
let logWindow = null;

function publishLog(text, reset = false) {
  operationLog = (reset ? text : operationLog + text).slice(-logLimit);
  const update = { revision: ++operationLogRevision, reset, text };
  for (const window of [mainWindow, logWindow]) {
    if (window && !window.isDestroyed()) {
      window.webContents.send("logs:update", update);
    }
  }
}

function loadRenderer(window, hash) {
  if (process.env.VITE_DEV_SERVER_URL) {
    const url = new URL(process.env.VITE_DEV_SERVER_URL);
    if (hash) url.hash = hash;
    void window.loadURL(url.href);
  } else {
    void window.loadFile(path.join(currentDirectory, "../build/renderer/index.html"), hash ? { hash } : {});
  }
}

const rendererPreferences = () => ({
  preload: path.join(currentDirectory, "preload.cjs"),
  contextIsolation: true,
  nodeIntegration: false,
  sandbox: true,
});

function openLogWindow() {
  if (logWindow && !logWindow.isDestroyed()) {
    if (logWindow.isMinimized()) logWindow.restore();
    logWindow.focus();
    return;
  }
  logWindow = new BrowserWindow({
    width: 1040,
    height: 680,
    minWidth: 760,
    minHeight: 360,
    backgroundColor: "#132126",
    title: "AudioLibraryToolkit logs",
    webPreferences: rendererPreferences(),
  });
  logWindow.on("closed", () => { logWindow = null; });
  loadRenderer(logWindow, "logs");
}

async function loadWorkerClient() {
  const moduleUrl = app.isPackaged
    ? pathToFileURL(path.join(process.resourcesPath, "worker-client", "index.js")).href
    : new URL("../../../packages/worker-client/dist/index.js", import.meta.url).href;
  ({ WorkerClient } = await import(moduleUrl));
}

function settingsFile() {
  return path.join(app.getPath("userData"), "ui-settings.json");
}

async function migrateLegacySettings() {
  const file = settingsFile();
  if (existsSync(file)) return;
  const legacy = path.join(app.getPath("appData"), "@audio-library", "desktop", "ui-settings.json");
  if (!existsSync(legacy)) return;
  await mkdir(path.dirname(file), { recursive: true });
  await copyFile(legacy, file);
}

async function getSettings() {
  await migrateLegacySettings();
  const file = settingsFile();
  if (!existsSync(file)) {
    return { backendPath: "", configPath: "", outputDirectory: "", libraryPath: "" };
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

function workerCandidates() {
  const executableName = process.platform === "win32" ? "AudioLibraryToolkit.exe" : "AudioLibraryToolkit";
  const buildPreset = process.platform === "win32" ? "windows-debug" : "linux-debug";
  const candidates = [
    path.join(path.dirname(process.execPath), executableName),
    path.join(process.resourcesPath, executableName),
    path.resolve(currentDirectory, "..", "..", "..", "..", "out", "build", buildPreset,
      "AudioLibraryToolkit", executableName),
    path.resolve(process.cwd(), executableName),
    ...(process.env.PATH ?? "").split(path.delimiter)
      .filter(Boolean).slice(0, 64).map((directory) => path.join(directory, executableName)),
  ];
  return [...new Set(candidates.map((candidate) => path.resolve(candidate)))];
}

async function probeBackend(candidate) {
  if (!existsSync(candidate)) return false;
  return new Promise((resolve) => {
    const child = spawn(candidate, ["--worker"], {
      stdio: ["pipe", "pipe", "ignore"],
      windowsHide: true,
    });
    const lines = createInterface({ input: child.stdout });
    let compatible = false;
    let settled = false;
    const finish = (result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      lines.close();
      resolve(result);
    };
    const timer = setTimeout(() => {
      child.kill();
      finish(false);
    }, 5000);
    lines.on("line", (line) => {
      try {
        const event = JSON.parse(line);
        if (event.type !== "ready" || event.protocolVersion !== 1 ||
            !Array.isArray(event.capabilities) || !event.capabilities.includes("readConfiguration")) {
          finish(false);
          child.kill();
          return;
        }
        compatible = true;
        child.stdin.end();
      } catch {
        finish(false);
        child.kill();
      }
    });
    child.on("error", () => finish(false));
    child.on("close", (code) => finish(compatible && code === 0));
  });
}

async function discoverBackend() {
  for (const candidate of workerCandidates()) {
    try {
      if (await probeBackend(candidate)) return candidate;
    } catch {
      // A candidate is selected only when it answers the worker protocol.
    }
  }
  return null;
}

async function runConfigurationOperation(operation, revision, patch) {
  const settings = await getSettings();
  if (!settings.backendPath) throw new Error("Choose or discover the AudioLibraryToolkit executable first.");
  if (!["readConfiguration", "validateConfiguration", "saveConfiguration"].includes(operation)) {
    throw new TypeError("Unsupported configuration operation.");
  }
  const parameters = {
    ...(settings.configPath.trim() ? { configPath: settings.configPath.trim() } : {}),
  };
  if (operation !== "readConfiguration") {
    if (typeof revision !== "string" || !revision ||
        typeof patch !== "object" || patch === null || Array.isArray(patch)) {
      throw new TypeError("A configuration revision and settings object are required.");
    }
    parameters.configRevision = revision;
    parameters.configurationPatch = JSON.stringify(patch);
  }
  const result = await new WorkerClient(settings.backendPath).run({ operation, parameters });
  if (result.status !== "succeeded") {
    throw new Error(result.stderr.trim() || "Backend configuration operation failed.");
  }
  const snapshot = JSON.parse(result.stdout);
  return snapshot;
}

async function createDefaultBackendConfiguration() {
  const settings = await getSettings();
  if (!settings.backendPath) throw new Error("Choose or discover the AudioLibraryToolkit executable first.");
  const parameters = settings.configPath.trim() ? { configPath: settings.configPath.trim() } : {};
  const result = await new WorkerClient(settings.backendPath).run({
    operation: "initializeDefaultConfig",
    parameters,
  });
  if (result.status !== "succeeded") {
    throw new Error(result.stderr.trim() || "Backend configuration creation failed.");
  }
  return runConfigurationOperation("readConfiguration");
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
  mainWindow = new BrowserWindow({
    width: 1440,
    height: 920,
    minWidth: 980,
    minHeight: 680,
    backgroundColor: "#17252a",
    title: "AudioLibraryToolkit",
    webPreferences: rendererPreferences(),
  });
  // The log window is auxiliary; it must not keep the application alive.
  mainWindow.on("closed", () => {
    mainWindow = null;
    if (logWindow && !logWindow.isDestroyed()) logWindow.close();
  });
  loadRenderer(mainWindow, "");
}

ipcMain.handle("settings:get", getSettings);
ipcMain.handle("settings:save", (_event, value) => saveSettings(value));
ipcMain.handle("operation:paths", (_event, libraryPath, outputDirectory) =>
  getOperationPaths(libraryPath, outputDirectory));
ipcMain.handle("backend:discover", discoverBackend);
ipcMain.handle("backend:configuration:read", () => runConfigurationOperation("readConfiguration"));
ipcMain.handle("backend:configuration:validate", (_event, revision, patch) =>
  runConfigurationOperation("validateConfiguration", revision, patch));
ipcMain.handle("backend:configuration:save", (_event, revision, patch) =>
  runConfigurationOperation("saveConfiguration", revision, patch));
ipcMain.handle("backend:configuration:create-default", createDefaultBackendConfiguration);
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
ipcMain.handle("logs:get", () => ({ revision: operationLogRevision, text: operationLog }));
ipcMain.handle("logs:open-window", () => openLogWindow());
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
  publishLog("", true);
  try {
    const settings = await getSettings().catch((error) => {
      publishLog(String(error instanceof Error ? error.message : error));
      throw error;
    });
    if (!settings.backendPath) {
      const message = "Choose the AudioLibraryToolkit backend executable in Settings.";
      publishLog(message);
      throw new Error(message);
    }

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
              if (workerEvent.type === "output") publishLog(workerEvent.text);
              if (!event.sender.isDestroyed()) {
                event.sender.send("operation:event", { requestId, event: workerEvent });
              }
            },
          },
        );
        if (result.stderr) publishLog("\n" + result.stderr);
        if (!event.sender.isDestroyed()) event.sender.send("operation:result", result);
      } catch (error) {
        const failure = {
          requestId,
          message: error instanceof Error ? error.message : String(error),
          diagnostics: error && typeof error === "object" && "diagnostics" in error
            ? String(error.diagnostics)
            : "",
        };
        publishLog("\n" + failure.message + "\n" + failure.diagnostics);
        if (!event.sender.isDestroyed()) event.sender.send("operation:error", failure);
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
