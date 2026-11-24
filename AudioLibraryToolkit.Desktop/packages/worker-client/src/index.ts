import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { randomUUID } from "node:crypto";
import { createInterface } from "node:readline";

export const WORKER_PROTOCOL_VERSION = 1;

export type WorkerOperation =
  | "convert"
  | "scan"
  | "duplicates"
  | "databaseImport"
  | "repair";

export type WorkerRequestOperation = WorkerOperation | "initializeConfig";

export type TagIssueKind =
  | "emptyValues"
  | "numericGenre"
  | "embeddedNul"
  | "flacOnlyBlocks"
  | "replayGain";

export type TagIssueAction = "apply" | "alternative" | "skipFiles" | "stop";

export interface DuplicateReviewPair {
  album1: string;
  album2: string;
  confidence: string;
  evidence: string[];
}

export interface DuplicateReviewGroup {
  albums: string[];
  pairs: DuplicateReviewPair[];
}

export interface DuplicateReport {
  status: string;
  statistics: Record<string, number>;
  groups: DuplicateReviewGroup[];
}

export interface WorkerOperationParameters {
  inputPath?: string;
  libraryPath?: string;
  jsonDirectory?: string;
  outputDirectory?: string;
  configPath?: string;
  target?: "flac" | "mp3";
  mp3Quality?: number;
  downmixToStereo?: boolean;
  overwrite?: boolean;
  noMetadataSidecar?: boolean;
  tagIssueResolutions?: Partial<Record<TagIssueKind, TagIssueAction>>;
  fullRescan?: boolean;
  tier?: "catalog" | "integrity" | "analysis";
  archiveFlacMetadata?: boolean;
  dryRun?: boolean;
  removeId3v1?: boolean;
  jobs?: number;
  rules?: string[];
  openFolders?: boolean;
  reportPath?: string;
  duplicateQuarantinePaths?: string[];
}

export interface WorkerOperationRequest {
  operation: WorkerRequestOperation;
  parameters: WorkerOperationParameters;
  mode?: "preview" | "run";
}

interface WorkerEventBase {
  protocolVersion: number;
  requestId: string;
  type: string;
}

export interface WorkerReadyEvent extends WorkerEventBase {
  type: "ready";
  capabilities: WorkerRequestOperation[];
}

export interface WorkerStartedEvent extends WorkerEventBase {
  type: "started";
}

export interface WorkerProgressEvent extends WorkerEventBase {
  type: "progress";
  message: string;
}

export interface WorkerOutputEvent extends WorkerEventBase {
  type: "output";
  stream: "stdout";
  text: string;
}

export interface WorkerCompletedEvent extends WorkerEventBase {
  type: "completed";
  exitCode: number;
  status: "succeeded" | "failed";
}

export interface WorkerErrorEvent extends WorkerEventBase {
  type: "error";
  message: string;
}

export type WorkerEvent =
  | WorkerReadyEvent
  | WorkerStartedEvent
  | WorkerProgressEvent
  | WorkerOutputEvent
  | WorkerCompletedEvent
  | WorkerErrorEvent;

export interface WorkerResult {
  requestId: string;
  status: "succeeded" | "failed" | "cancelled";
  exitCode: number | null;
  stdout: string;
  stderr: string;
}

export interface RunOptions {
  requestId?: string;
  signal?: AbortSignal;
  onEvent?: (event: WorkerEvent) => void;
  env?: NodeJS.ProcessEnv;
}

export class WorkerClientError extends Error {
  constructor(message: string, readonly diagnostics = "") {
    super(message);
    this.name = "WorkerClientError";
  }
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

export function parseWorkerEvent(line: string): WorkerEvent {
  let value: unknown;
  try {
    value = JSON.parse(line) as unknown;
  } catch (error) {
    throw new WorkerClientError(
      `Backend emitted invalid JSON: ${error instanceof Error ? error.message : String(error)}`,
    );
  }
  if (!isObject(value) || value.protocolVersion !== WORKER_PROTOCOL_VERSION ||
      typeof value.requestId !== "string" || typeof value.type !== "string") {
    throw new WorkerClientError("Backend emitted an invalid or incompatible protocol event.");
  }
  switch (value.type) {
    case "ready":
      if (!Array.isArray(value.capabilities) ||
          value.capabilities.some((item) => typeof item !== "string")) {
        throw new WorkerClientError("Backend ready event has invalid capabilities.");
      }
      return value as unknown as WorkerReadyEvent;
    case "started":
      return value as unknown as WorkerStartedEvent;
    case "progress":
      if (typeof value.message !== "string") throw new WorkerClientError("Invalid progress event.");
      return value as unknown as WorkerProgressEvent;
    case "output":
      if (value.stream !== "stdout" || typeof value.text !== "string") {
        throw new WorkerClientError("Invalid output event.");
      }
      return value as unknown as WorkerOutputEvent;
    case "completed":
      if (typeof value.exitCode !== "number" ||
          (value.status !== "succeeded" && value.status !== "failed")) {
        throw new WorkerClientError("Invalid completion event.");
      }
      return value as unknown as WorkerCompletedEvent;
    case "error":
      if (typeof value.message !== "string") throw new WorkerClientError("Invalid error event.");
      return value as unknown as WorkerErrorEvent;
    default:
      throw new WorkerClientError(`Unknown backend event type: ${value.type}`);
  }
}

export class WorkerClient {
  constructor(
    private readonly executablePath: string,
    private readonly baseEnvironment: NodeJS.ProcessEnv = process.env,
  ) {
    if (!executablePath.trim()) throw new TypeError("Backend executable path is required.");
  }

  run(
    request: WorkerOperationRequest,
    options: RunOptions = {},
  ): Promise<WorkerResult> {
    const requestId = options.requestId ?? randomUUID();
    const signal = options.signal;
    const child = spawn(this.executablePath, ["--worker"], {
      stdio: ["pipe", "pipe", "pipe"],
      windowsHide: true,
      detached: process.platform !== "win32",
      env: { ...this.baseEnvironment, ...options.env },
    });
    return new Promise<WorkerResult>((resolve, reject) => {
      let completed: WorkerCompletedEvent | undefined;
      let ready = false;
      let cancelled = signal?.aborted ?? false;
      let settled = false;
      let stdout = "";
      let stderr = "";
      let protocolFailure: Error | undefined;
      const lines = createInterface({ input: child.stdout });

      const finishError = (error: Error) => {
        if (settled) return;
        settled = true;
        reject(error);
      };
      const stopWorker = () => {
        if (child.pid === undefined) return;
        try {
          if (process.platform === "win32") child.kill();
          else process.kill(-child.pid, "SIGTERM");
        } catch (error) {
          if (!(error instanceof Error) || !("code" in error) || error.code !== "ESRCH") {
            finishError(new WorkerClientError(
              `Could not stop backend worker: ${error instanceof Error ? error.message : String(error)}`,
              stderr,
            ));
          }
        }
      };
      const abort = () => {
        cancelled = true;
        stopWorker();
      };
      if (signal) {
        if (signal.aborted) abort();
        else signal.addEventListener("abort", abort, { once: true });
      }

      child.stderr.setEncoding("utf8");
      child.stderr.on("data", (chunk: string) => {
        stderr = (stderr + chunk).slice(-65_536);
      });
      child.stdin.on("error", (error) => {
        protocolFailure = new WorkerClientError(`Could not send request to backend: ${error.message}`, stderr);
        stopWorker();
      });

      lines.on("line", (line) => {
        try {
          const event = parseWorkerEvent(line);
          if (event.type === "ready") {
            if (ready) throw new WorkerClientError("Backend sent more than one ready event.", stderr);
            ready = true;
            if (cancelled) {
              child.stdin.end();
              stopWorker();
            } else {
              child.stdin.write(JSON.stringify({
                protocolVersion: WORKER_PROTOCOL_VERSION,
                requestId,
                type: request.mode ?? "run",
                operation: request.operation,
                parameters: request.parameters,
              }) + "\n");
              child.stdin.end();
            }
          } else if (event.requestId !== requestId) {
            throw new WorkerClientError("Backend event requestId does not match the active request.", stderr);
          } else if (event.type === "output") {
            stdout = (stdout + event.text).slice(-1_048_576);
          } else if (event.type === "completed") {
            completed = event;
          } else if (event.type === "error") {
            throw new WorkerClientError(event.message, stderr);
          }
          options.onEvent?.(event);
        } catch (error) {
          protocolFailure = error instanceof Error ? error : new Error(String(error));
          stopWorker();
        }
      });

      child.on("error", (error) => {
        finishError(new WorkerClientError(`Could not start backend: ${error.message}`, stderr));
      });
      child.on("close", (code, terminationSignal) => {
        signal?.removeEventListener("abort", abort);
        if (settled) return;
        if (protocolFailure) {
          finishError(new WorkerClientError(protocolFailure.message, stderr));
          return;
        }
        if (completed) {
          settled = true;
          resolve({
            requestId,
            status: completed.status,
            exitCode: completed.exitCode,
            stdout,
            stderr,
          });
          return;
        }
        if (cancelled) {
          settled = true;
          resolve({ requestId, status: "cancelled", exitCode: null, stdout, stderr });
          return;
        }
        if (!completed) {
          finishError(new WorkerClientError(
            `Backend exited without a completion event (code ${String(code)}, signal ${String(terminationSignal)}).`,
            stderr,
          ));
          return;
        }
      });
    });
  }
}
