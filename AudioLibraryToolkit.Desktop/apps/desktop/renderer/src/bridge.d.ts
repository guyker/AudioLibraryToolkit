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
}

export interface BackendConfigurationSnapshot {
  schemaVersion: 1;
  path: string;
  exists: boolean;
  revision: string;
  configuration: Record<string, unknown>;
}

interface OperationError {
  requestId: string;
  message: string;
  diagnostics: string;
}

interface AudioLibraryBridge {
  getSettings(): Promise<DesktopSettings>;
  saveSettings(settings: DesktopSettings): Promise<DesktopSettings>;
  getOperationPaths(libraryPath: string, outputDirectory: string): Promise<{
    catalogDirectory: string;
    reportPath: string;
  }>;
  pickDirectory(): Promise<string | null>;
  pickFile(kind: "backend" | "config"): Promise<string | null>;
  discoverBackend(): Promise<string | null>;
  readBackendConfiguration(): Promise<BackendConfigurationSnapshot>;
  validateBackendConfiguration(
    revision: string,
    patch: Record<string, unknown>,
  ): Promise<BackendConfigurationSnapshot>;
  saveBackendConfiguration(
    revision: string,
    patch: Record<string, unknown>,
  ): Promise<BackendConfigurationSnapshot>;
  createDefaultBackendConfiguration(): Promise<BackendConfigurationSnapshot>;
  saveDuplicateReportPath(): Promise<string | null>;
  readDuplicateReport(reportPath: string): Promise<DuplicateReport>;
  openAlbumFolder(albumPath: string): Promise<void>;
  startOperation(request: {
    requestId: string;
    operation: WorkerOperation | "initializeConfig";
    mode?: "preview" | "run";
    parameters: WorkerOperationParameters;
  }): Promise<string>;
  cancelOperation(requestId: string): Promise<boolean>;
  getLogs(): Promise<{ revision: number; text: string }>;
  openLogWindow(): Promise<void>;
  onLogUpdate(handler: (update: { revision: number; reset: boolean; text: string }) => void): () => void;
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
