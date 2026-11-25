import { useEffect, useRef, useState } from "react";
import { subscribeToLogs } from "./logStream.mjs";

export function latestLogLine(text: string): string {
  const lines = text.split(/\r?\n|\r/);
  for (let index = lines.length - 1; index >= 0; index -= 1) {
    const line = lines[index].trim();
    if (line) return line.length > 240 ? `${line.slice(0, 237)}…` : line;
  }
  return "";
}

interface LogViewerProps {
  text: string;
  onClose: () => void;
  onOpenWindow?: () => void;
  closeLabel?: string;
}

export function LogViewer({ text, onClose, onOpenWindow, closeLabel = "Close" }: LogViewerProps) {
  const [follow, setFollow] = useState(true);
  const [copyMessage, setCopyMessage] = useState("");
  const outputRef = useRef<HTMLPreElement>(null);
  const lineCount = text ? text.split(/\r?\n|\r/).filter((line) => line.trim()).length : 0;

  useEffect(() => {
    outputRef.current?.focus();
  }, []);

  useEffect(() => {
    const output = outputRef.current;
    if (follow && output) output.scrollTop = output.scrollHeight;
  }, [text, follow]);

  function onScroll() {
    const output = outputRef.current;
    if (!output) return;
    setFollow(output.scrollHeight - output.scrollTop - output.clientHeight < 24);
  }

  async function copyLogs() {
    try {
      await navigator.clipboard.writeText(text);
      setCopyMessage("Copied");
    } catch {
      setCopyMessage("Copy failed");
    }
    window.setTimeout(() => setCopyMessage(""), 1800);
  }

  return (
    <div className="log-viewer">
      <div className="log-toolbar">
        <h2 id="log-title">Logs</h2>
        <span className="log-count">
          {lineCount} {lineCount === 1 ? "line" : "lines"} from the last operation (latest 200,000 characters)
        </span>
        <div className="log-actions">
          <span className="log-copy-message" role="status">{copyMessage}</span>
          {!follow && (
            <button className="quiet-button" onClick={() => setFollow(true)} type="button">
              Jump to latest
            </button>
          )}
          <button className="quiet-button" disabled={!text} onClick={() => void copyLogs()} type="button">
            Copy logs
          </button>
          {onOpenWindow && (
            <button className="quiet-button" onClick={onOpenWindow} type="button">
              Open in new window
            </button>
          )}
          <button className="quiet-button" onClick={onClose} type="button">
            {closeLabel}
          </button>
        </div>
      </div>
      <pre ref={outputRef} tabIndex={0} aria-label="Worker output" onScroll={onScroll}>
        {text || "Operation output and diagnostics appear here when an operation runs."}
      </pre>
    </div>
  );
}

export function LogWindow() {
  const [text, setText] = useState("");
  const [error, setError] = useState("");

  useEffect(() => {
    document.title = "AudioLibraryToolkit logs";
    return subscribeToLogs(window.audioLibrary, setText, (reason: unknown) => {
      setError(reason instanceof Error ? reason.message : String(reason));
    });
  }, []);

  return (
    <div className="log-window">
      {error && <div className="log-error" role="alert">Could not load logs: {error}</div>}
      <LogViewer text={text} onClose={() => window.close()} closeLabel="Close window" />
    </div>
  );
}
