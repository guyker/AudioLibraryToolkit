import "@fontsource/ibm-plex-sans/400.css";
import "@fontsource/ibm-plex-sans/500.css";
import "@fontsource/ibm-plex-sans/600.css";
import "@fontsource/ibm-plex-mono/400.css";
import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import { LogWindow } from "./LogViewer";
import "./styles.css";

createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    {window.location.hash === "#logs" ? <LogWindow /> : <App />}
  </React.StrictMode>,
);
