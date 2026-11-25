import { useEffect, useMemo, useRef, useState } from "react";
import type {
  TagIssueAction,
  TagIssueKind,
  DuplicateReport,
  WorkerEvent,
  WorkerOperation,
  WorkerOperationParameters,
} from "@audio-library/worker-client";
import type { BackendConfigurationSnapshot } from "./bridge";
import { LogViewer, latestLogLine } from "./LogViewer";
import { subscribeToLogs } from "./logStream.mjs";

type Screen = WorkerOperation | "settings";
type RunState = "idle" | "running" | "succeeded" | "failed" | "cancelled";

interface Settings {
  backendPath: string;
  configPath: string;
  outputDirectory: string;
  libraryPath: string;
}

const operationInfo: Record<WorkerOperation, { title: string; description: string; verb: string }> = {
  convert: {
    title: "Convert audio",
    description: "Create a verified FLAC or MP3 copy in a separate output tree.",
    verb: "Start conversion",
  },
  scan: {
    title: "Build a catalog",
    description: "Read a library and write searchable JSON metadata.",
    verb: "Start scan",
  },
  duplicates: {
    title: "Find duplicates",
    description: "Compare generated catalog data and inspect matching albums.",
    verb: "Find duplicate albums",
  },
  databaseImport: {
    title: "Import to SQLite",
    description: "Load catalog JSON into a library-specific SQLite database.",
    verb: "Import catalog",
  },
  repair: {
    title: "Repair metadata",
    description: "Apply safe FLAC/MP3 metadata repairs without re-encoding audio.",
    verb: "Review and repair",
  },
};

const tagIssueFields: {
  key: TagIssueKind;
  label: string;
  choices: { value: TagIssueAction; label: string }[];
}[] = [
  {
    key: "emptyValues",
    label: "Empty tag values",
    choices: [{ value: "apply", label: "Drop empty values" }],
  },
  {
    key: "numericGenre",
    label: "Numeric genres",
    choices: [
      { value: "apply", label: "Convert ID3 genre numbers to names" },
      { value: "alternative", label: "Keep numbers as written" },
    ],
  },
  {
    key: "embeddedNul",
    label: "Values containing NUL characters",
    choices: [
      { value: "apply", label: "Split into separate values" },
      { value: "alternative", label: "Replace NUL with \"; \"" },
    ],
  },
  {
    key: "flacOnlyBlocks",
    label: "FLAC-only metadata blocks",
    choices: [{ value: "apply", label: "Leave out of MP3 (source unchanged)" }],
  },
  {
    key: "replayGain",
    label: "ReplayGain/R128 tags",
    choices: [
      { value: "apply", label: "Keep the tags" },
      { value: "alternative", label: "Remove the tags from MP3" },
    ],
  },
];

const emptySettings: Settings = { backendPath: "", configPath: "", outputDirectory: "", libraryPath: "" };

const configurationGroups: Record<string, string[]> = {
  "Library & paths": ["OutDirectory", "DatabaseFileName", "MediaDirectoryList"],
  Scanning: ["Incremental", "ScanFlacMetadataArchives", "ScanTier", "CatalogExtensions"],
  Conversion: ["FLACSettings", "MetadataSidecars", "TagIssueResolution"],
  Duplicates: [
    "MinMatchingTracksForDuplicate", "SizeMatchPercentageThreshold",
    "DuplicateDurationToleranceSeconds", "DuplicateFuzzyNames",
    "DuplicateStripEditionText", "DuplicatePartialMatches", "DuplicatePartialMinCoverage",
  ],
  Performance: [
    "UseAsyncMetadataReads", "MetadataScanMaxThreads", "DecodeWorkers",
    "ExtraAudioQualityMetrics", "AlbumsSplitThreshold", "JsonBatchTargetBytes",
    "RecursionDirectorySearchDepth",
  ],
  Advanced: ["Version", "LogLevel"],
};

const complexConfigurationKeys = new Set([
  "MediaDirectoryList", "CatalogExtensions", "FLACSettings", "MetadataSidecars",
]);

function mediaLibraryPath(configuration: Record<string, unknown>): string {
  const entries = configuration.MediaDirectoryList;
  if (!Array.isArray(entries)) return "";
  const firstActive = entries.find((entry) =>
    typeof entry === "object" && entry !== null && "isActive" in entry &&
    entry.isActive === true && "mediaPath" in entry && typeof entry.mediaPath === "string");
  return firstActive && typeof firstActive === "object" && "mediaPath" in firstActive
    ? String(firstActive.mediaPath) : "";
}

function isMutating(screen: WorkerOperation, parameters: WorkerOperationParameters): boolean {
  if (screen === "duplicates") return typeof parameters.reportPath === "string" && parameters.reportPath.length > 0;
  return !(screen === "repair" && parameters.dryRun);
}

function operationParameters(
  screen: WorkerOperation,
  inputPath: string,
  outputDirectory: string,
  options: Record<string, string | boolean | number>,
  tagIssueResolutions: Partial<Record<TagIssueKind, TagIssueAction>>,
  duplicateQuarantinePaths: string[] = [],
): WorkerOperationParameters {
  const output = outputDirectory ? { outputDirectory } : {};
  switch (screen) {
    case "convert":
      return {
        inputPath,
        ...output,
        target: options.target as "flac" | "mp3",
        ...(options.target === "mp3" ? { mp3Quality: Number(options.mp3Quality) } : {}),
        ...(options.target === "mp3" && Object.keys(tagIssueResolutions).length > 0
          ? { tagIssueResolutions }
          : {}),
        downmixToStereo: options.downmixToStereo === true,
        overwrite: options.overwrite === true,
        noMetadataSidecar: options.noMetadataSidecar === true,
      };
    case "scan":
      return {
        inputPath,
        ...output,
        tier: options.tier as "catalog" | "integrity" | "analysis",
        fullRescan: options.fullRescan === true,
        archiveFlacMetadata: options.archiveFlacMetadata === true,
      };
    case "duplicates":
      return {
        jsonDirectory: inputPath,
        openFolders: options.openFolders === true,
        ...(typeof options.reportPath === "string" && options.reportPath.trim()
          ? { reportPath: options.reportPath.trim() }
          : {}),
        ...(duplicateQuarantinePaths.length > 0 ? { duplicateQuarantinePaths } : {}),
      };
    case "databaseImport":
      return { jsonDirectory: inputPath, ...output };
    case "repair":
      return {
        inputPath,
        ...output,
        dryRun: options.dryRun === true,
        removeId3v1: options.removeId3v1 === true,
        overwrite: options.overwrite === true,
        jobs: Number(options.jobs),
        rules: typeof options.rules === "string"
          ? options.rules.split(",").map((rule) => rule.trim()).filter(Boolean)
          : [],
      };
  }
}

function eventText(event: WorkerEvent): string {
  switch (event.type) {
    case "started":
      return "Backend accepted the operation.";
    case "progress":
      return event.message;
    case "output":
      return event.text;
    case "completed":
      return `Operation ${event.status} (exit code ${event.exitCode}).`;
    case "error":
      return event.message;
    case "ready":
      return `Backend ready; ${event.capabilities.length} operations available.`;
  }
}

export function App() {
  const [screen, setScreen] = useState<Screen>("convert");
  const [settings, setSettings] = useState<Settings>(emptySettings);
  const [settingsDraft, setSettingsDraft] = useState<Settings>(emptySettings);
  const [settingsMessage, setSettingsMessage] = useState("");
  const [audioInputPath, setAudioInputPath] = useState("");
  const [catalogInputPath, setCatalogInputPath] = useState("");
  const [outputDirectory, setOutputDirectory] = useState("");
  const [configuredOutputDirectory, setConfiguredOutputDirectory] = useState("");
  const [configurationSnapshot, setConfigurationSnapshot] =
    useState<BackendConfigurationSnapshot | null>(null);
  const [configurationDraft, setConfigurationDraft] = useState<Record<string, unknown>>({});
  const [configurationText, setConfigurationText] = useState<Record<string, string>>({});
  const [configurationTab, setConfigurationTab] = useState("Library & paths");
  const [configurationMessage, setConfigurationMessage] = useState("");
  const [discoveringBackend, setDiscoveringBackend] = useState(false);
  const [options, setOptions] = useState<Record<string, string | boolean | number>>({
    target: "flac",
    mp3Quality: 0,
    tier: "catalog",
    jobs: 4,
  });
  const [tagIssueResolutions, setTagIssueResolutions] =
    useState<Partial<Record<TagIssueKind, TagIssueAction>>>({});
  const [duplicateReport, setDuplicateReport] = useState<DuplicateReport | null>(null);
  const [selectedQuarantinePaths, setSelectedQuarantinePaths] = useState<string[]>([]);
  const [runState, setRunState] = useState<RunState>("idle");
  const [runMessage, setRunMessage] = useState("Choose an operation to begin.");
  const [runOutput, setRunOutput] = useState("");
  const [logsOpen, setLogsOpen] = useState(false);
  const logSheetRef = useRef<HTMLElement>(null);
  const viewLogsButtonRef = useRef<HTMLButtonElement>(null);
  const [activeRequest, setActiveRequest] = useState("");
  const activeRequestRef = useRef("");
  const activeModeRef = useRef<"preview" | "run">("run");
  const activeOperationRef = useRef<WorkerOperation | "initializeConfig" | null>(null);
  const activeReportPathRef = useRef("");
  const activeQuarantinePathsRef = useRef<string[]>([]);
  const [previewReady, setPreviewReady] = useState(false);
  const [errorMessage, setErrorMessage] = useState("");

  const selectedOperation = screen === "settings" ? null : screen;
  const usesCatalogInput = selectedOperation === "duplicates" || selectedOperation === "databaseImport";
  const inputPath = usesCatalogInput ? catalogInputPath : audioInputPath;
  const metadata = selectedOperation ? operationInfo[selectedOperation] : null;
  const inputLabel = usesCatalogInput
    ? "Catalog JSON directory" : "Source library";

  function setInputPath(value: string) {
    if (usesCatalogInput) setCatalogInputPath(value);
    else setAudioInputPath(value);
  }

  useEffect(() => {
    void window.audioLibrary.getSettings().then((value) => {
      setSettings(value);
      setSettingsDraft(value);
      setOutputDirectory(value.outputDirectory);
      setAudioInputPath(value.libraryPath);
      if (value.backendPath) {
        void loadBackendConfiguration(value.backendPath, value.configPath, false);
      } else {
        setScreen("settings");
        setConfigurationMessage("Choose or discover the backend, then use an existing configuration or create one from defaults.");
      }
    }).catch((error: unknown) => {
      setErrorMessage(error instanceof Error ? error.message : String(error));
    });
  }, []);

  useEffect(() => {
    return subscribeToLogs(window.audioLibrary, setRunOutput, (error: unknown) => {
      setErrorMessage(`Could not load logs: ${error instanceof Error ? error.message : String(error)}`);
    });
  }, []);

  useEffect(() => {
    const unsubscribeEvent = window.audioLibrary.onOperationEvent(({ requestId, event }) => {
      if (requestId !== activeRequestRef.current) return;
      if (event.type !== "output" && event.type !== "ready") {
        setRunMessage(eventText(event));
      }
    });
    const unsubscribeResult = window.audioLibrary.onOperationResult((result) => {
      if (result.requestId !== activeRequestRef.current) return;
      if (activeModeRef.current === "preview" && result.status === "succeeded") {
        setPreviewReady(true);
      }
      if (activeOperationRef.current === "duplicates" && result.status === "succeeded") {
        if (activeQuarantinePathsRef.current.length > 0) {
          setDuplicateReport(null);
          setSelectedQuarantinePaths([]);
        } else if (activeReportPathRef.current) {
          void window.audioLibrary.readDuplicateReport(activeReportPathRef.current)
            .then((report) => {
              setDuplicateReport(report);
              setSelectedQuarantinePaths([]);
            })
            .catch((error: unknown) => {
              setErrorMessage(error instanceof Error ? error.message : String(error));
            });
        }
      }
      setRunState(result.status);
      setRunMessage(result.status === "cancelled"
        ? "Stopped. Completed outputs are retained; inspect the destination for partial work."
        : result.status === "failed" ? "Operation failed. Open the logs for details."
          : activeModeRef.current === "preview"
            ? "Preview complete. Review the planned changes in the logs, then run the operation."
            : "Operation completed.");
      activeRequestRef.current = "";
      activeModeRef.current = "run";
      activeOperationRef.current = null;
      activeReportPathRef.current = "";
      activeQuarantinePathsRef.current = [];
      setActiveRequest("");
    });
    const unsubscribeError = window.audioLibrary.onOperationError((failure) => {
      if (failure.requestId !== activeRequestRef.current) return;
      setRunState("failed");
      setRunMessage(failure.message);
      setErrorMessage(failure.message);
      activeRequestRef.current = "";
      activeModeRef.current = "run";
      activeOperationRef.current = null;
      activeReportPathRef.current = "";
      activeQuarantinePathsRef.current = [];
      setActiveRequest("");
    });
    return () => {
      unsubscribeEvent();
      unsubscribeResult();
      unsubscribeError();
    };
  }, []);

  useEffect(() => {
    setPreviewReady(false);
  }, [screen, inputPath, outputDirectory, options, tagIssueResolutions]);

  useEffect(() => {
    setDuplicateReport(null);
    setSelectedQuarantinePaths([]);
  }, [inputPath, options.reportPath]);

  const isRunning = runState === "running" && activeRequest.length > 0;
  const progressPercent = useMemo(() => {
    const match = runMessage.match(/\((\d{1,3})%\)/);
    return match ? Math.min(Number(match[1]), 100) : null;
  }, [runMessage]);
  const latestOutput = useMemo(() => latestLogLine(runOutput), [runOutput]);

  useEffect(() => {
    if (!logsOpen) return;
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        setLogsOpen(false);
      } else if (event.key === "Tab") {
        const elements = logSheetRef.current?.querySelectorAll<HTMLElement>("button:not(:disabled), pre[tabindex]");
        if (!elements?.length) return;
        const first = elements[0];
        const last = elements[elements.length - 1];
        if (event.shiftKey && document.activeElement === first) {
          event.preventDefault();
          last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault();
          first.focus();
        }
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => {
      window.removeEventListener("keydown", onKeyDown);
      viewLogsButtonRef.current?.focus();
    };
  }, [logsOpen]);

  async function openLogWindow() {
    setLogsOpen(false);
    try {
      await window.audioLibrary.openLogWindow();
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : String(error));
    }
  }

  function setOption(name: string, value: string | boolean | number) {
    setOptions((current) => ({ ...current, [name]: value }));
  }

  async function pickDirectory(setValue: (value: string) => void) {
    const value = await window.audioLibrary.pickDirectory();
    if (value) setValue(value);
  }

  async function pickFile(kind: "backend" | "config", setValue: (value: string) => void) {
    const value = await window.audioLibrary.pickFile(kind);
    if (value) setValue(value);
  }

  async function discoverBackend() {
    setConfigurationMessage("");
    setDiscoveringBackend(true);
    try {
      const discovered = await window.audioLibrary.discoverBackend();
      if (!discovered) {
        setConfigurationMessage("No compatible AudioLibraryToolkit executable was found. Choose it manually.");
        return;
      }
      const next = { ...settingsDraft, backendPath: discovered };
      const saved = await window.audioLibrary.saveSettings(next);
      setSettings(saved);
      setSettingsDraft(saved);
      await loadBackendConfiguration(discovered, saved.configPath, false);
    } catch (error) {
      setConfigurationMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setDiscoveringBackend(false);
    }
  }

  async function loadBackendConfiguration(
    backendPath = settingsDraft.backendPath,
    configPath = settingsDraft.configPath,
    saveDesktopSettings = true,
  ) {
    setConfigurationMessage("");
    try {
      if (!backendPath.trim()) throw new Error("Choose or discover the backend executable first.");
      if (saveDesktopSettings) {
        const saved = await window.audioLibrary.saveSettings({
          ...settingsDraft, backendPath, configPath,
        });
        setSettings(saved);
        setSettingsDraft(saved);
      }
      const snapshot = await window.audioLibrary.readBackendConfiguration();
      setConfigurationSnapshot(snapshot);
      setConfigurationDraft(snapshot.configuration);
      const textValues: Record<string, string> = {};
      for (const key of complexConfigurationKeys) {
        textValues[key] = JSON.stringify(snapshot.configuration[key] ?? null, null, 2);
      }
      setConfigurationText(textValues);
      const configuredOutput = typeof snapshot.configuration.OutDirectory === "string"
        ? snapshot.configuration.OutDirectory : "";
      setConfiguredOutputDirectory(configuredOutput);
      setOutputDirectory(configuredOutput);
      const configuredLibrary = mediaLibraryPath(snapshot.configuration);
      const savedLibrary = settingsDraft.libraryPath;
      if (configuredLibrary || savedLibrary) {
        const libraryPath = configuredLibrary || savedLibrary;
        setSettingsDraft((current) => ({ ...current, libraryPath }));
        setInputPath(libraryPath);
      }
      setConfigurationMessage(snapshot.exists
        ? `Loaded backend configuration from ${snapshot.path}.`
        : `No backend file exists at ${snapshot.path}. Defaults are available but have not been saved.`);
      if (!snapshot.exists) setScreen("settings");
    } catch (error) {
      setConfigurationSnapshot(null);
      setConfigurationMessage(error instanceof Error ? error.message : String(error));
    }
  }

  async function createDefaultBackendConfiguration() {
    if (!settingsDraft.backendPath.trim()) {
      setConfigurationMessage("Choose or discover the backend executable first.");
      return;
    }
    setConfigurationMessage("");
    try {
      const saved = await window.audioLibrary.saveSettings(settingsDraft);
      setSettings(saved);
      setSettingsDraft(saved);
      const created = await window.audioLibrary.createDefaultBackendConfiguration();
      setConfigurationSnapshot(created);
      setConfigurationDraft(created.configuration);
      const output = typeof created.configuration.OutDirectory === "string"
        ? created.configuration.OutDirectory : "";
      setConfiguredOutputDirectory(output);
      setOutputDirectory(output);
      setConfigurationMessage(`Created backend configuration at ${created.path}.`);
    } catch (error) {
      setConfigurationMessage(error instanceof Error ? error.message : String(error));
    }
  }

  async function saveBackendConfiguration() {
    if (!configurationSnapshot) return;
    setConfigurationMessage("");
    try {
      const patch: Record<string, unknown> = {};
      for (const key of Object.keys(configurationDraft)) {
        const value = complexConfigurationKeys.has(key)
          ? JSON.parse(configurationText[key] ?? "null") as unknown
          : configurationDraft[key];
        if (JSON.stringify(value) !== JSON.stringify(configurationSnapshot.configuration[key])) {
          patch[key] = value;
        }
      }
      if (Object.keys(patch).length === 0) {
        setConfigurationMessage("No backend settings have changed.");
        return;
      }
      await window.audioLibrary.validateBackendConfiguration(configurationSnapshot.revision, patch);
      const saved = await window.audioLibrary.saveBackendConfiguration(configurationSnapshot.revision, patch);
      setConfigurationSnapshot(saved);
      setConfigurationDraft(saved.configuration);
      setConfigurationMessage(`Saved backend configuration to ${saved.path}.`);
      const output = typeof saved.configuration.OutDirectory === "string"
        ? saved.configuration.OutDirectory : "";
      setConfiguredOutputDirectory(output);
      setOutputDirectory(output);
      const library = mediaLibraryPath(saved.configuration);
      if (library) {
        setInputPath(library);
        setSettingsDraft((current) => ({ ...current, libraryPath: library }));
      }
    } catch (error) {
      setConfigurationMessage(error instanceof Error ? error.message : String(error));
    }
  }

  function setConfigurationValue(key: string, value: unknown) {
    setConfigurationDraft((current) => ({ ...current, [key]: value }));
  }

  async function saveSettings() {
    setSettingsMessage("");
    try {
      const saved = await window.audioLibrary.saveSettings(settingsDraft);
      setSettings(saved);
      if (!configurationSnapshot) setOutputDirectory(saved.outputDirectory);
      setSettingsMessage("Settings saved.");
      if (saved.backendPath) {
        await loadBackendConfiguration(saved.backendPath, saved.configPath, false);
      }
    } catch (error) {
      setSettingsMessage(error instanceof Error ? error.message : String(error));
    }
  }

  async function createBackendConfiguration() {
    if (!window.confirm(
      "Create the backend configuration using its built-in defaults?\n\n" +
      "An existing configuration will never be overwritten.")) return;
    await createDefaultBackendConfiguration();
  }

  async function startOperation() {
    if (!selectedOperation || !inputPath.trim()) {
      setErrorMessage("Choose an input directory before starting.");
      return;
    }
    const outputOverride = outputDirectory.trim() === configuredOutputDirectory.trim()
      ? "" : outputDirectory.trim();
    const parameters = operationParameters(
      selectedOperation, inputPath.trim(), outputOverride, options, tagIssueResolutions,
      selectedQuarantinePaths);
    const previewable = selectedOperation === "convert" || selectedOperation === "repair";
    const mode = previewable && !previewReady ? "preview" : "run";
    if (mode === "run" && isMutating(selectedOperation, parameters)) {
      const target = outputDirectory.trim() || "the configured output directory";
      const accepted = window.confirm(
        `${metadata?.title}\n\nInput: ${inputPath.trim()}\nOutput root: ${target}\n\n` +
        "Sources are not intentionally modified. Existing destinations may be rejected unless overwrite is enabled. Continue?");
      if (!accepted) return;
    }
    setErrorMessage("");
    setRunState("running");
    setRunMessage(mode === "preview" ? "Preparing a backend preview…" : "Starting the backend worker…");
    const requestId = crypto.randomUUID();
    activeRequestRef.current = requestId;
    activeModeRef.current = mode;
    activeOperationRef.current = selectedOperation;
    activeReportPathRef.current = selectedOperation === "duplicates" &&
      typeof parameters.reportPath === "string" ? parameters.reportPath : "";
    activeQuarantinePathsRef.current = parameters.duplicateQuarantinePaths ?? [];
    setActiveRequest(requestId);
    try {
      await window.audioLibrary.startOperation({
        requestId,
        operation: selectedOperation,
        mode,
        parameters,
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      activeRequestRef.current = "";
      activeModeRef.current = "run";
      activeOperationRef.current = null;
      setActiveRequest("");
      setRunState("failed");
      setRunMessage(message);
    }
  }

  async function chooseDuplicateReportPath() {
    try {
      const reportPath = await window.audioLibrary.saveDuplicateReportPath();
      if (reportPath) setOption("reportPath", reportPath);
    } catch (error) {
      setErrorMessage(error instanceof Error ? error.message : String(error));
    }
  }

  function setAlbumSelected(group: DuplicateReport["groups"][number], albumPath: string, checked: boolean) {
    setSelectedQuarantinePaths((current) => {
      if (!checked) return current.filter((path) => path !== albumPath);
      const selectedInGroup = group.albums.filter((path) => current.includes(path)).length;
      if (selectedInGroup >= group.albums.length - 1) return current;
      return [...current, albumPath];
    });
  }

  async function quarantineSelectedAlbums() {
    if (!selectedQuarantinePaths.length || !inputPath.trim() || !outputDirectory.trim()) {
      setErrorMessage("Select albums to quarantine and set an output root.");
      return;
    }
    if (!window.confirm(
      `Quarantine ${selectedQuarantinePaths.length} selected album folder(s)?\n\n` +
      "The backend creates and verifies a complete copy before removing originals. " +
      "Catalog JSON and databases are not updated.")) {
      return;
    }
    setErrorMessage("");
    setRunState("running");
    setRunMessage("Starting duplicate review…");
    const requestId = crypto.randomUUID();
    activeRequestRef.current = requestId;
    activeModeRef.current = "run";
    activeOperationRef.current = "duplicates";
    activeReportPathRef.current = "";
    activeQuarantinePathsRef.current = [...selectedQuarantinePaths];
    setActiveRequest(requestId);
    try {
      await window.audioLibrary.startOperation({
        requestId,
        operation: "duplicates",
        parameters: {
          jsonDirectory: inputPath.trim(),
          ...(outputDirectory.trim() !== configuredOutputDirectory.trim()
            ? { outputDirectory: outputDirectory.trim() } : {}),
          duplicateQuarantinePaths: [...selectedQuarantinePaths],
        },
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      activeRequestRef.current = "";
      activeModeRef.current = "run";
      activeOperationRef.current = null;
      activeReportPathRef.current = "";
      activeQuarantinePathsRef.current = [];
      setActiveRequest("");
      setRunState("failed");
      setRunMessage(message);
    }
  }

  async function cancelOperation() {
    if (!activeRequest || !window.confirm(
      "Stop this worker now? Verified outputs already installed remain; temporary files may need inspection.")) {
      return;
    }
    setRunMessage("Stopping the backend worker…");
    await window.audioLibrary.cancelOperation(activeRequest);
  }

  return (
    <div className="shell">
      <aside className="rail">
        <div className="brand">
          <div className="brand-mark" aria-hidden="true"><span /><span /><span /></div>
          <div><strong>AudioLibrary</strong><small>Toolkit</small></div>
        </div>
        <div className="rail-caption">Library work</div>
        <nav aria-label="Operations">
          {(Object.keys(operationInfo) as WorkerOperation[]).map((operation) => (
            <button
              className={`nav-item ${screen === operation ? "selected" : ""}`}
              key={operation}
              onClick={() => setScreen(operation)}
              type="button"
            >
              <span className={`nav-glyph glyph-${operation}`} aria-hidden="true" />
              <span>{operationInfo[operation].title}</span>
            </button>
          ))}
        </nav>
        <div className="rail-bottom">
          <div className="backend-state">
            <span className={`state-dot ${settings.backendPath ? "online" : ""}`} />
            <span>{settings.backendPath ? "Backend selected" : "Backend not set"}</span>
          </div>
          <button
            className={`nav-item settings-link ${screen === "settings" ? "selected" : ""}`}
            onClick={() => setScreen("settings")}
            type="button"
          >
            <span className="settings-glyph" aria-hidden="true">⚙</span>
            <span>Settings</span>
          </button>
          <p className="version-label">Worker protocol 1</p>
        </div>
      </aside>

      <main className="workbench">
        <section className="run-status" aria-label="Operation status">
          <span className={`status-mark mark-${runState}`} aria-hidden="true">
            {runState === "succeeded" ? "✓" : runState === "failed" ? "!" : runState === "cancelled" ? "–" : runState === "running" ? "…" : "·"}
          </span>
          <div className="run-status-copy" role="status">
            <h2>Run status</h2>
            <p>{runMessage}</p>
            {latestOutput && (
              <p className="run-status-detail" title={latestOutput}>{latestOutput}</p>
            )}
          </div>
          {isRunning && (
            <button className="quiet-button danger-text" onClick={() => void cancelOperation()} type="button">
              Stop worker
            </button>
          )}
          <button
            ref={viewLogsButtonRef}
            className={`quiet-button ${runState === "failed" ? "attention-button" : ""}`}
            aria-haspopup="dialog"
            onClick={() => setLogsOpen(true)}
            type="button"
          >
            View logs
          </button>
          {isRunning && (
            <div className={`progress-track ${progressPercent === null ? "indeterminate" : ""}`}>
              <span style={progressPercent === null ? undefined : { transform: `scaleX(${progressPercent / 100})` }} />
            </div>
          )}
        </section>
        <header className="page-head">
          <div>
            <div className="breadcrumb">AUDIO LIBRARY / {screen === "settings" ? "SETTINGS" : "WORKSPACE"}</div>
            <h1>{metadata?.title ?? "Settings"}</h1>
            <p>{metadata?.description ?? "Choose the backend executable and local paths used by this desktop client."}</p>
          </div>
          <div className={`run-chip run-${runState}`}>
            <span className="state-dot" />
            {runState === "running" ? "Working" : runState === "succeeded" ? "Complete" :
              runState === "failed" ? "Needs attention" : runState === "cancelled" ? "Stopped" : "Ready"}
          </div>
        </header>

        {errorMessage && (
          <div className="alert" role="alert">
            <strong>Could not continue</strong>
            <span>{errorMessage}</span>
            <button type="button" onClick={() => setErrorMessage("")} aria-label="Dismiss error">×</button>
          </div>
        )}

        {screen === "settings" ? (
          <section className="settings-page">
            <div className="section-heading">
              <h2>Application settings</h2>
              <p>The desktop keeps its connection preferences separately. Backend settings are read and edited in place.</p>
            </div>
            <div className="settings-tabs" role="tablist" aria-label="Settings groups">
              {["Backend connection", ...Object.keys(configurationGroups)].map((tab) => (
                <button
                  aria-selected={configurationTab === tab}
                  className={configurationTab === tab ? "active" : ""}
                  key={tab}
                  onClick={() => setConfigurationTab(tab)}
                  role="tab"
                  type="button"
                >
                  {tab}
                </button>
              ))}
            </div>
            {configurationTab === "Backend connection" ? (
              <div className="configuration-panel">
                <p className="configuration-note">
                  The desktop launches the headless C++ worker as a separate process. FFmpeg remains a backend prerequisite.
                </p>
                <div className="path-field">
                  <PathField
                    label="AudioLibraryToolkit executable"
                    value={settingsDraft.backendPath}
                    placeholder="Select AudioLibraryToolkit.exe or the Linux executable"
                    onChange={(value) => setSettingsDraft((current) => ({ ...current, backendPath: value }))}
                    onBrowse={() => pickFile("backend", (value) => {
                      setSettingsDraft((current) => ({ ...current, backendPath: value }));
                    })}
                    browseLabel="Choose file"
                  />
                  <button
                    className="quiet-button"
                    disabled={discoveringBackend}
                    onClick={() => void discoverBackend()}
                    type="button"
                  >
                    {discoveringBackend ? "Searching…" : "Discover executable"}
                  </button>
                </div>
                <PathField
                  label="Optional configuration file"
                  value={settingsDraft.configPath}
                  placeholder="Blank uses the backend's per-user config.json"
                  onChange={(value) => setSettingsDraft((current) => ({ ...current, configPath: value }))}
                  onBrowse={() => pickFile("config", (value) => {
                    setSettingsDraft((current) => ({ ...current, configPath: value }));
                  })}
                  browseLabel="Choose file"
                />
                <div className="configuration-actions">
                  <button
                    className="quiet-button"
                    onClick={() => void loadBackendConfiguration()}
                    type="button"
                  >
                    Use existing configuration
                  </button>
                  <button
                    className="quiet-button"
                    onClick={() => void createBackendConfiguration()}
                    type="button"
                  >
                    Create with backend defaults
                  </button>
                  <button className="primary-button" onClick={() => void saveSettings()} type="button">
                    Save desktop settings
                  </button>
                </div>
                {configurationSnapshot && (
                  <p className="configuration-location">
                    {configurationSnapshot.exists ? "Using" : "No file exists yet at"}{" "}
                    <code>{configurationSnapshot.path}</code>
                  </p>
                )}
              </div>
            ) : (
              <div className="configuration-panel" role="tabpanel">
                <div className="section-heading">
                  <h3>{configurationTab}</h3>
                  <p>These values are validated by the backend before any file is changed.</p>
                </div>
                {configurationSnapshot ? (
                  <>
                    {configurationGroups[configurationTab].map((key) => (
                      <ConfigurationField
                        key={key}
                        name={key}
                        value={configurationDraft[key]}
                        textValue={configurationText[key] ?? ""}
                        onValueChange={(value) => setConfigurationValue(key, value)}
                        onTextChange={(value) => setConfigurationText((current) => ({ ...current, [key]: value }))}
                      />
                    ))}
                    <div className="configuration-actions">
                      <button className="primary-button" onClick={() => void saveBackendConfiguration()} type="button">
                        Validate and save backend settings
                      </button>
                      <span className="safety-note">Comments and unknown keys in the file are preserved.</span>
                    </div>
                  </>
                ) : (
                  <p className="configuration-note">Choose a backend and use its existing configuration, or create one from defaults.</p>
                )}
              </div>
            )}
            {(configurationMessage || settingsMessage) && (
              <p className="inline-message" role="status">{configurationMessage || settingsMessage}</p>
            )}
          </section>
        ) : (
          <div className="workspace-grid">
            <section className="operation-form" aria-labelledby="operation-form-title">
              <div className="section-heading">
                <h2 id="operation-form-title">Operation details</h2>
                <p>Choose the paths and settings for this run. The backend checks them again before writing.</p>
              </div>
              <PathField
                label={inputLabel}
                value={inputPath}
                placeholder={selectedOperation === "duplicates" || selectedOperation === "databaseImport"
                  ? "Path to generated catalog JSON" : "Path to the audio library"}
                onChange={setInputPath}
                onBrowse={() => pickDirectory(setInputPath)}
                browseLabel="Browse"
              />
              {selectedOperation !== "duplicates" && (
                <PathField
                  label="Output root"
                  value={outputDirectory}
                  placeholder="Defaults to the output root in backend config"
                  onChange={setOutputDirectory}
                  onBrowse={() => pickDirectory(setOutputDirectory)}
                  browseLabel="Browse"
                />
              )}
              {selectedOperation === "convert" && (
                <div className="field-grid">
                  <label className="field">
                    <span>Output format</span>
                    <select value={String(options.target)} onChange={(event) => setOption("target", event.target.value)}>
                      <option value="flac">FLAC</option><option value="mp3">MP3</option>
                    </select>
                  </label>
                  {options.target === "mp3" && (
                    <label className="field">
                      <span>LAME VBR quality</span>
                      <select value={String(options.mp3Quality)} onChange={(event) => setOption("mp3Quality", Number(event.target.value))}>
                        {Array.from({ length: 10 }, (_, quality) => <option value={quality} key={quality}>{quality}{quality === 0 ? " · highest" : ""}</option>)}
                      </select>
                    </label>
                  )}
                  {options.target === "mp3" && <CheckField label="Allow multichannel downmix to stereo" checked={options.downmixToStereo === true} onChange={(value) => setOption("downmixToStereo", value)} />}
                  <CheckField label="Replace existing output files" checked={options.overwrite === true} onChange={(value) => setOption("overwrite", value)} />
                  <CheckField label="Disable optional metadata sidecars" checked={options.noMetadataSidecar === true} onChange={(value) => setOption("noMetadataSidecar", value)} />
                </div>
              )}
              {selectedOperation === "convert" && options.target === "mp3" && (
                <section className="tag-resolution">
                  <div className="section-heading">
                    <h3>MP3 tag decisions</h3>
                    <p>These choices apply only when the matching issue is detected. Blank choices remain unresolved; the configured automatic policy may apply recommended fixes, otherwise conversion stops before writing.</p>
                  </div>
                  <div className="field-grid">
                    {tagIssueFields.map((field) => (
                      <label className="field" key={field.key}>
                        <span>{field.label}</span>
                        <select
                          value={tagIssueResolutions[field.key] ?? ""}
                          onChange={(event) => {
                            const value = event.target.value as TagIssueAction | "";
                            setTagIssueResolutions((current) => {
                              const next = { ...current };
                              if (value) next[field.key] = value;
                              else delete next[field.key];
                              return next;
                            });
                          }}
                        >
                          <option value="">Leave unresolved</option>
                          {field.choices.map((choice) => (
                            <option value={choice.value} key={choice.value}>{choice.label}</option>
                          ))}
                          <option value="skipFiles">Skip affected files</option>
                          <option value="stop">Stop without converting</option>
                        </select>
                      </label>
                    ))}
                  </div>
                </section>
              )}
              {selectedOperation === "scan" && (
                <div className="field-grid">
                  <label className="field">
                    <span>Scan depth</span>
                    <select value={String(options.tier)} onChange={(event) => setOption("tier", event.target.value)}>
                      <option value="catalog">Catalog only</option>
                      <option value="integrity">Integrity checks</option>
                      <option value="analysis">Audio analysis</option>
                    </select>
                  </label>
                  <CheckField label="Read every file again" checked={options.fullRescan === true} onChange={(value) => setOption("fullRescan", value)} />
                  <CheckField label="Archive original FLAC metadata" checked={options.archiveFlacMetadata === true} onChange={(value) => setOption("archiveFlacMetadata", value)} />
                </div>
              )}
              {selectedOperation === "duplicates" && (
                <div className="field-grid">
                  <CheckField label="Open matching album folders" checked={options.openFolders === true} onChange={(value) => setOption("openFolders", value)} />
                  <label className="field field-wide">
                    <span>New JSON report path (required to review groups)</span>
                    <div className="path-control">
                      <input value={String(options.reportPath ?? "")} onChange={(event) => setOption("reportPath", event.target.value)} placeholder="Choose a new report file; existing files are not overwritten" />
                      <button className="browse-button" onClick={() => void chooseDuplicateReportPath()} type="button">Choose file</button>
                    </div>
                  </label>
                </div>
              )}
              {selectedOperation === "duplicates" && duplicateReport && (
                <section className="tag-resolution duplicate-review">
                  <div className="section-heading">
                    <h3>Duplicate groups</h3>
                    <p>
                      {duplicateReport.statistics.pairsReported ?? 0} matching pair(s) in {duplicateReport.groups.length} group(s).
                      Select folders to quarantine; keep at least one album in each group.
                    </p>
                  </div>
                  {duplicateReport.groups.length === 0 ? (
                    <p className="inline-message">No duplicate groups were found.</p>
                  ) : (
                    <>
                      <div className="duplicate-groups">
                        {duplicateReport.groups.map((group, groupIndex) => {
                          const selectedInGroup = group.albums.filter((album) =>
                            selectedQuarantinePaths.includes(album)).length;
                          return (
                            <article className="duplicate-group" key={groupIndex}>
                              <h4>Group {groupIndex + 1}</h4>
                              <div className="duplicate-albums">
                                {group.albums.map((album) => {
                                  const checked = selectedQuarantinePaths.includes(album);
                                  return (
                                    <label className="duplicate-album" key={album}>
                                      <input
                                        type="checkbox"
                                        checked={checked}
                                        disabled={!checked && selectedInGroup >= group.albums.length - 1}
                                        onChange={(event) => setAlbumSelected(group, album, event.target.checked)}
                                      />
                                      <span>{album}</span>
                                    </label>
                                  );
                                })}
                              </div>
                              <div className="duplicate-pairs">
                                {group.pairs.map((pair, pairIndex) => (
                                  <div className="duplicate-pair" key={`${pair.album1}-${pair.album2}-${pairIndex}`}>
                                    <strong>{pair.confidence}</strong>
                                    <span>{pair.album1} matches {pair.album2}</span>
                                    {pair.evidence.length > 0 && <small>{pair.evidence.join("; ")}</small>}
                                  </div>
                                ))}
                              </div>
                            </article>
                          );
                        })}
                      </div>
                      <div className="form-actions">
                        <button
                          className="primary-button"
                          disabled={isRunning || selectedQuarantinePaths.length === 0}
                          onClick={() => void quarantineSelectedAlbums()}
                          type="button"
                        >
                          Quarantine {selectedQuarantinePaths.length} selected
                        </button>
                        <span className="safety-note">The backend rechecks each group and verifies copies before source removal.</span>
                      </div>
                    </>
                  )}
                </section>
              )}
              {selectedOperation === "repair" && (
                <div className="field-grid">
                  <label className="field">
                    <span>Parallel workers</span>
                    <select value={String(options.jobs)} onChange={(event) => setOption("jobs", Number(event.target.value))}>
                      {[1, 2, 4, 8, 16].map((jobs) => <option value={jobs} key={jobs}>{jobs}</option>)}
                    </select>
                  </label>
                  <CheckField label="Dry run only (write no repair output)" checked={options.dryRun === true} onChange={(value) => setOption("dryRun", value)} />
                  <CheckField label="Remove redundant MP3 ID3v1 tags" checked={options.removeId3v1 === true} onChange={(value) => setOption("removeId3v1", value)} />
                  <CheckField label="Replace existing repair output" checked={options.overwrite === true} onChange={(value) => setOption("overwrite", value)} />
                  <label className="field field-wide">
                    <span>Limit to rule IDs (comma-separated, blank means all default rules)</span>
                    <input value={String(options.rules ?? "")} onChange={(event) => setOption("rules", event.target.value)} placeholder="e.g. flac.duplicate-comments" />
                  </label>
                </div>
              )}
              {selectedOperation === "databaseImport" && (
                <div className="notice-line"><span className="notice-mark">i</span> Imports update the library database transactionally; existing legacy databases are left untouched.</div>
              )}
              <div className="form-actions">
                <button className="primary-button" disabled={isRunning} onClick={() => void startOperation()} type="button">
                  {selectedOperation && (selectedOperation === "convert" || selectedOperation === "repair") && !previewReady
                    ? "Preview operation"
                    : metadata?.verb ?? "Start operation"}
                </button>
                <span className="safety-note">
                  {previewReady ? "Preview complete. The backend checks the plan again before writing." :
                    "The backend owns validation and file-safety checks."}
                </span>
              </div>
            </section>
          </div>
        )}
      </main>
      {logsOpen && (
        <div
          className="log-overlay"
          onMouseDown={(event) => { if (event.target === event.currentTarget) setLogsOpen(false); }}
        >
          <section ref={logSheetRef} className="log-sheet" role="dialog" aria-modal="true" aria-labelledby="log-title">
            <LogViewer
              text={runOutput}
              onClose={() => setLogsOpen(false)}
              onOpenWindow={() => void openLogWindow()}
            />
          </section>
        </div>
      )}
    </div>
  );
}

interface PathFieldProps {
  label: string;
  value: string;
  placeholder: string;
  onChange: (value: string) => void;
  onBrowse: () => void;
  browseLabel: string;
}

function PathField({ label, value, placeholder, onChange, onBrowse, browseLabel }: PathFieldProps) {
  return (
    <label className="field path-field">
      <span>{label}</span>
      <div className="path-control">
        <input value={value} onChange={(event) => onChange(event.target.value)} placeholder={placeholder} />
        <button className="browse-button" onClick={onBrowse} type="button">{browseLabel}</button>
      </div>
    </label>
  );
}

function ConfigurationField({
  name, value, textValue, onValueChange, onTextChange,
}: {
  name: string;
  value: unknown;
  textValue: string;
  onValueChange: (value: unknown) => void;
  onTextChange: (value: string) => void;
}) {
  if (complexConfigurationKeys.has(name)) {
    return (
      <label className="field configuration-field">
        <span>{name}</span>
        <textarea
          spellCheck={false}
          value={textValue}
          onChange={(event) => onTextChange(event.target.value)}
        />
      </label>
    );
  }
  if (typeof value === "boolean") {
    return (
      <CheckField label={name} checked={value} onChange={(checked) => onValueChange(checked)} />
    );
  }
  if (typeof value === "number") {
    return (
      <label className="field configuration-field">
        <span>{name}</span>
        <input
          type="number"
          value={Number.isFinite(value) ? value : ""}
          onChange={(event) => onValueChange(event.target.value === "" ? "" : event.target.valueAsNumber)}
        />
      </label>
    );
  }
  return (
    <label className="field configuration-field">
      <span>{name}</span>
      <input
        value={typeof value === "string" ? value : ""}
        onChange={(event) => onValueChange(event.target.value)}
      />
    </label>
  );
}

function CheckField({ label, checked, onChange }: { label: string; checked: boolean; onChange: (value: boolean) => void }) {
  return (
    <label className="check-field">
      <input type="checkbox" checked={checked} onChange={(event) => onChange(event.target.checked)} />
      <span className="check-indicator" aria-hidden="true" />
      <span>{label}</span>
    </label>
  );
}
