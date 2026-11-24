const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("audioLibrary", {
  getSettings: () => ipcRenderer.invoke("settings:get"),
  saveSettings: (settings) => ipcRenderer.invoke("settings:save", settings),
  pickDirectory: () => ipcRenderer.invoke("dialog:directory"),
  pickFile: (kind) => ipcRenderer.invoke("dialog:file", kind),
  saveDuplicateReportPath: () => ipcRenderer.invoke("dialog:save-duplicate-report"),
  readDuplicateReport: (reportPath) => ipcRenderer.invoke("duplicates:read-report", reportPath),
  startOperation: (request) => ipcRenderer.invoke("operation:start", request),
  cancelOperation: (requestId) => ipcRenderer.invoke("operation:cancel", requestId),
  onOperationEvent: (handler) => {
    const listener = (_event, payload) => handler(payload);
    ipcRenderer.on("operation:event", listener);
    return () => ipcRenderer.removeListener("operation:event", listener);
  },
  onOperationResult: (handler) => {
    const listener = (_event, payload) => handler(payload);
    ipcRenderer.on("operation:result", listener);
    return () => ipcRenderer.removeListener("operation:result", listener);
  },
  onOperationError: (handler) => {
    const listener = (_event, payload) => handler(payload);
    ipcRenderer.on("operation:error", listener);
    return () => ipcRenderer.removeListener("operation:error", listener);
  },
});
