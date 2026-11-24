"""Verify the machine-readable worker boundary using temporary inputs."""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="alt-worker-") as directory:
        root = Path(directory)
        input_dir = root / "input"
        input_dir.mkdir()
        json_dir = root / "empty-catalog"
        json_dir.mkdir()
        config = root / "config.json"
        config.write_text(json.dumps({"OutDirectory": str(root / "output")}), encoding="utf-8")

        requests = [
            {"protocolVersion": 2, "requestId": "bad-version", "type": "run",
             "operation": "duplicates", "parameters": {"jsonDirectory": str(json_dir)}},
            {"protocolVersion": 1, "requestId": "bad-parameter", "type": "run",
             "operation": "scan", "parameters": {"inputPath": str(root), "tier": "invalid"}},
            {"protocolVersion": 1, "requestId": "bad-operation", "type": "run",
             "operation": "execute-unknown", "parameters": {"inputPath": str(root)}},
            {"protocolVersion": 1, "requestId": "bad-tag-resolution", "type": "run",
             "operation": "convert",
             "parameters": {"inputPath": str(input_dir), "target": "mp3",
                            "tagIssueResolutions": {"numericGenre": "guess"}}},
            {"protocolVersion": 1, "requestId": "invalid-preview", "type": "preview",
             "operation": "scan", "parameters": {"inputPath": str(input_dir)}},
            {"protocolVersion": 1, "requestId": "bad-quarantine-paths", "type": "run",
             "operation": "duplicates",
             "parameters": {"jsonDirectory": str(json_dir), "duplicateQuarantinePaths": "album"}},
            {"protocolVersion": 1, "requestId": "initialize-config", "type": "run",
             "operation": "initializeConfig",
             "parameters": {"libraryPath": str(input_dir),
                            "outputDirectory": str(root / "first-output"),
                            "configPath": str(root / "first-run" / "config.json")}},
            {"protocolVersion": 1, "requestId": "initialize-config-existing", "type": "run",
             "operation": "initializeConfig",
             "parameters": {"libraryPath": str(input_dir),
                            "outputDirectory": str(root / "replacement-output"),
                            "configPath": str(root / "first-run" / "config.json")}},
            {"protocolVersion": 1, "requestId": "valid-repair", "type": "run",
             "operation": "repair",
             "parameters": {"inputPath": str(input_dir), "outputDirectory": str(root / "output"),
                            "dryRun": True, "configPath": str(config)}},
        ]
        env = os.environ.copy()
        env["LOCALAPPDATA"] = str(root / "local-app-data")
        result = subprocess.run(
            [str(executable), "--worker"],
            input="".join(json.dumps(item, ensure_ascii=False) + "\n" for item in requests),
            capture_output=True, text=True, encoding="utf-8", env=env, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        events = [json.loads(line) for line in result.stdout.splitlines()]
        assert events[0]["type"] == "ready" and events[0]["protocolVersion"] == 1, events
        errors = {event.get("requestId"): event for event in events if event["type"] == "error"}
        assert "Unsupported or missing protocolVersion" in errors["bad-version"]["message"], errors
        assert "tier must be" in errors["bad-parameter"]["message"], errors
        assert "Unsupported operation" in errors["bad-operation"]["message"], errors
        assert "Invalid tag issue resolution" in errors["bad-tag-resolution"]["message"], errors
        assert "Preview is supported only" in errors["invalid-preview"]["message"], errors
        assert "duplicateQuarantinePaths must be an array" in errors["bad-quarantine-paths"]["message"], errors
        initialized = next(event for event in events
                           if event["type"] == "completed" and event["requestId"] == "initialize-config")
        assert initialized["status"] == "succeeded", (initialized, result.stderr)
        refused_overwrite = next(event for event in events
                                 if event["type"] == "completed" and
                                 event["requestId"] == "initialize-config-existing")
        assert refused_overwrite["status"] == "failed", (refused_overwrite, result.stderr)
        first_config = (root / "first-run" / "config.json").read_text(encoding="utf-8")
        assert json.dumps(str(root / "first-output"))[1:-1] in first_config, first_config
        assert json.dumps(str(root / "replacement-output"))[1:-1] not in first_config, first_config
        assert json.dumps(str(input_dir))[1:-1] in first_config, first_config
        completed = next(event for event in events
                         if event["type"] == "completed" and event["requestId"] == "valid-repair")
        assert completed["status"] == "succeeded", (completed, result.stderr)
        assert all(event.get("protocolVersion") == 1 for event in events), events


if __name__ == "__main__":
    main()
