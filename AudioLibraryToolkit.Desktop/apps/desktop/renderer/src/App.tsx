import { useEffect, useMemo, useRef, useState } from "react";
import type {
  TagIssueAction,
  TagIssueKind,
  DuplicateReport,
  WorkerEvent,
  WorkerOperation,
  WorkerOperationParameters,
} from "@audio-library/worker-client";

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
  const [inputPath, setInputPath] = useState("");
  const [outputDirectory, setOutputDirectory] = useState("");
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
  const [activeRequest, setActiveRequest] = useState("");
  const activeRequestRef = useRef("");
  const activeModeRef = useRef<"preview" | "run">("run");
  const activeOperationRef = useRef<WorkerOperation | "initializeConfig" | null>(null);
  const activeReportPathRef = useRef("");
  const activeQuarantinePathsRef = useRef<string[]>([]);
  const [previewReady, setPreviewReady] = useState(false);
  const [errorMessage, setErrorMessage] = useState("");

  const selectedOperation = screen === "settings" ? null : screen;
  const metadata = selectedOperation ? operationInfo[selectedOperation] : null;
  const inputLabel = selectedOperation === "duplicates" || selectedOperation === "databaseImport"
    ? "Catalog JSON directory" : "Source library";

  useEffect(() => {
    void window.audioLibrary.getSettings().then((value) => {
      setSettings(value);
      setSettingsDraft(value);
      setOutputDirectory(value.outputDirectory);
    }).catch((error: unknown) => {
      setErrorMessage(error instanceof Error ? error.message : String(error));
    });
  }, []);

  useEffect(() => {
    const unsubscribeEvent = window.audioLibrary.onOperationEvent(({ requestId, event }) => {
      if (requestId !== activeRequestRef.current) return;
      setRunMessage(eventText(event));
      if (event.type === "output") {
        setRunOutput((current) => (current + event.text).slice(-200_000));
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
        : result.status === "succeeded" ? "Operation completed." : "Operation failed.");
      if (result.stderr) {
        setRunOutput((current) => (current + "\n" + result.stderr).slice(-200_000));
      }
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
      setRunOutput((current) => (current + "\n" + failure.diagnostics).slice(-200_000));
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

  async function saveSettings() {
    setSettingsMessage("");
    try {
      const saved = await window.audioLibrary.saveSettings(settingsDraft);
      setSettings(saved);
      setOutputDirectory(saved.outputDirectory);
      setSettingsMessage("Settings saved.");
    } catch (error) {
      setSettingsMessage(error instanceof Error ? error.message : String(error));
    }
  }

  async function createBackendConfiguration() {
    if (!settingsDraft.backendPath.trim() || !settingsDraft.outputDirectory.trim() ||
        !settingsDraft.libraryPath.trim()) {
      setSettingsMessage("Choose the backend, output directory, and source library before setup.");
      return;
    }
    if (!window.confirm(
      "Create a new backend configuration?\n\n" +
      "The configuration file will not overwrite an existing file. It will save the output directory and source library.")) {
      return;
    }
    setSettingsMessage("");
    setErrorMessage("");
    try {
      const saved = await window.audioLibrary.saveSettings(settingsDraft);
      setSettings(saved);
      setSettingsDraft(saved);
      setOutputDirectory(saved.outputDirectory);
      const requestId = crypto.randomUUID();
      activeRequestRef.current = requestId;
      activeModeRef.current = "run";
      setActiveRequest(requestId);
      setRunOutput("");
      setRunState("running");
      setRunMessage("Creating the backend configuration…");
      await window.audioLibrary.startOperation({
        requestId,
        operation: "initializeConfig",
        parameters: {
          libraryPath: saved.libraryPath,
          outputDirectory: saved.outputDirectory,
          ...(saved.configPath.trim() ? { configPath: saved.configPath.trim() } : {}),
        },
      });
    } catch (error) {
      activeRequestRef.current = "";
      activeModeRef.current = "run";
      activeOperationRef.current = null;
      setActiveRequest("");
      setRunState("failed");
      setRunMessage(error instanceof Error ? error.message : String(error));
      setSettingsMessage(error instanceof Error ? error.message : String(error));
    }
  }

  async function startOperation() {
    if (!selectedOperation || !inputPath.trim()) {
      setErrorMessage("Choose an input directory before starting.");
      return;
    }
    const parameters = operationParameters(
      selectedOperation, inputPath.trim(), outputDirectory.trim(), options, tagIssueResolutions,
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
    setRunOutput("");
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
      activeRequestRef.current = "";
      activeModeRef.current = "run";
      activeOperationRef.current = null;
      setActiveRequest("");
      setRunState("failed");
      setRunMessage(error instanceof Error ? error.message : String(error));
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
    setRunOutput("");
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
          outputDirectory: outputDirectory.trim(),
          duplicateQuarantinePaths: [...selectedQuarantinePaths],
        },
      });
    } catch (error) {
      activeRequestRef.current = "";
      activeModeRef.current = "run";
      activeOperationRef.current = null;
      activeReportPathRef.current = "";
      activeQuarantinePathsRef.current = [];
      setActiveRequest("");
      setRunState("failed");
      setRunMessage(error instanceof Error ? error.message : String(error));
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
              <h2>Backend connection</h2>
              <p>The desktop app launches the C++ worker as a separate process. No DLL or C++ ABI is shared.</p>
            </div>
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
            <PathField
              label="Optional configuration file"
              value={settingsDraft.configPath}
              placeholder="Uses the backend's per-user config when blank"
              onChange={(value) => setSettingsDraft((current) => ({ ...current, configPath: value }))}
              onBrowse={() => pickFile("config", (value) => {
                setSettingsDraft((current) => ({ ...current, configPath: value }));
              })}
              browseLabel="Choose file"
            />
            <PathField
              label="Default output root"
              value={settingsDraft.outputDirectory}
              placeholder="Choose a destination outside the source library"
              onChange={(value) => setSettingsDraft((current) => ({ ...current, outputDirectory: value }))}
              onBrowse={() => pickDirectory((value) => {
                setSettingsDraft((current) => ({ ...current, outputDirectory: value }));
              })}
              browseLabel="Browse"
            />
            <PathField
              label="Initial source library"
              value={settingsDraft.libraryPath}
              placeholder="Choose an existing audio library folder"
              onChange={(value) => setSettingsDraft((current) => ({ ...current, libraryPath: value }))}
              onBrowse={() => pickDirectory((value) => {
                setSettingsDraft((current) => ({ ...current, libraryPath: value }));
              })}
              browseLabel="Browse"
            />
            <div className="settings-foot">
              <p>FFmpeg remains a backend prerequisite; this UI installer does not bundle it.</p>
              <button className="quiet-button" onClick={() => void createBackendConfiguration()} type="button">
                Create backend configuration
              </button>
              <button className="primary-button" onClick={() => void saveSettings()} type="button">Save settings</button>
            </div>
            {settingsMessage && <p className="inline-message" role="status">{settingsMessage}</p>}
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
                {isRunning && <button className="quiet-button danger-text" onClick={() => void cancelOperation()} type="button">Stop worker</button>}
                <span className="safety-note">
                  {previewReady ? "Preview complete. The backend checks the plan again before writing." :
                    "The backend owns validation and file-safety checks."}
                </span>
              </div>
            </section>

            <aside className="run-panel" aria-label="Operation status">
              <div className="run-panel-head">
                <div><h2>Run status</h2><p>{runState === "idle" ? "No operation started" : runMessage}</p></div>
                <span className={`status-mark mark-${runState}`} aria-hidden="true">{runState === "succeeded" ? "✓" : runState === "failed" ? "!" : runState === "cancelled" ? "–" : runState === "running" ? "…" : "·"}</span>
              </div>
              {isRunning && <div className={`progress-track ${progressPercent === null ? "indeterminate" : ""}`}><span style={progressPercent === null ? undefined : { transform: `scaleX(${progressPercent / 100})` }} /></div>}
              <div className="run-copy">
                <div className="run-copy-head"><span>Worker output</span><span>{settings.backendPath ? "Connected by process" : "Backend required"}</span></div>
                <pre aria-live="polite">{runOutput || "Progress and operation output will appear here."}</pre>
              </div>
              <div className="run-footer">
                <span className={`state-dot ${settings.backendPath ? "online" : ""}`} />
                <span>{settings.backendPath || "Set the backend executable in Settings"}</span>
              </div>
            </aside>
          </div>
        )}
      </main>
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

function CheckField({ label, checked, onChange }: { label: string; checked: boolean; onChange: (value: boolean) => void }) {
  return (
    <label className="check-field">
      <input type="checkbox" checked={checked} onChange={(event) => onChange(event.target.checked)} />
      <span className="check-indicator" aria-hidden="true" />
      <span>{label}</span>
    </label>
  );
}
