interface LogSnapshot {
  revision: number;
  text: string;
}

interface LogUpdate extends LogSnapshot {
  reset: boolean;
}

interface LogBridge {
  getLogs(): Promise<LogSnapshot>;
  onLogUpdate(handler: (update: LogUpdate) => void): () => void;
}

export function subscribeToLogs(
  bridge: LogBridge,
  onChange: (text: string) => void,
  onError: (error: unknown) => void,
): () => void;
