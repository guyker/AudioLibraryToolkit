import type {
  DuplicateReport,
  WorkerEvent,
  WorkerOperation,
  WorkerOperationParameters,
  WorkerResult,
} from "@audio-library/worker-client";

interface DesktopSettings {
  backendPath: string;
  configPath: string;
  outputDirectory: string;
  libraryPath: string;
  libraryPath: string;
}

interface OperationError {
  requestId: string;
  message: string;
  diagnostics: string;
}

interface AudioLibraryBridge {
  getSettings(): Promise<DesktopSettings>;
  saveSettings(settings: DesktopSettings): Promise<DesktopSettings>;
  pickDirectory(): Promise<string | null>;
  pickFile(kind: "backend" | "config"): Promise<string | null>;
  saveDuplicateReportPath(): Promise<string | null>;
  readDuplicateReport(reportPath: string): Promise<DuplicateReport>;
  startOperation(request: {
    requestId: string;
    operation: WorkerOperation | "initializeConfig";
    mode?: "preview" | "run";
    parameters: WorkerOperationParameters;
  }): Promise<string>;
  cancelOperation(requestId: string): Promise<boolean>;
  onOperationEvent(handler: (payload: { requestId: string; event: WorkerEvent }) => void): () => void;
  onOperationResult(handler: (result: WorkerResult) => void): () => void;
  onOperationError(handler: (error: OperationError) => void): () => void;
}

declare global {
  interface Window {
    audioLibrary: AudioLibraryBridge;
  }
}

export {};
