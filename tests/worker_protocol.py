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
        assert "initializeDefaultConfig" in events[0]["capabilities"], events[0]
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

        editable_config = root / "editable.json"
        editable_config.write_text(
            '{\n  // retain this explanation\n  "OutDirectory": "old-output",\n'
            '  "LogLevel": 1,\n  "UnknownSetting": { "preserve": true }\n}\n',
            encoding="utf-8")

        def call_config(request):
            completed = subprocess.run(
                [str(executable), "--worker"],
                input=json.dumps(request) + "\n",
                capture_output=True, text=True, encoding="utf-8", env=env, timeout=60)
            assert completed.returncode == 0, completed.stdout + completed.stderr
            emitted = [json.loads(line) for line in completed.stdout.splitlines()]
            response = next(item for item in emitted
                            if item.get("requestId") == request["requestId"] and
                            item["type"] == "completed")
            return emitted, response

        read_request = {
            "protocolVersion": 1, "requestId": "config-read", "type": "run",
            "operation": "readConfiguration", "parameters": {"configPath": str(editable_config)},
        }
        read_events, read_completed = call_config(read_request)
        assert read_completed["status"] == "succeeded", read_events
        read_text = "".join(item.get("text", "") for item in read_events
                            if item.get("type") == "output")
        snapshot = json.loads(read_text)
        assert snapshot["exists"] and snapshot["revision"] != "missing", snapshot

        invalid_request = {
            "protocolVersion": 1, "requestId": "config-invalid", "type": "run",
            "operation": "validateConfiguration",
            "parameters": {
                "configPath": str(editable_config), "configRevision": snapshot["revision"],
                "configurationPatch": json.dumps({"LogLevel": 99}),
            },
        }
        _, invalid_completed = call_config(invalid_request)
        assert invalid_completed["status"] == "failed", invalid_completed
        assert "old-output" in editable_config.read_text(encoding="utf-8")

        save_request = {
            "protocolVersion": 1, "requestId": "config-save", "type": "run",
            "operation": "saveConfiguration",
            "parameters": {
                "configPath": str(editable_config), "configRevision": snapshot["revision"],
                "configurationPatch": json.dumps({"OutDirectory": "new-output"}),
            },
        }
        save_events, save_completed = call_config(save_request)
        assert save_completed["status"] == "succeeded", save_events
        saved = json.loads("".join(item.get("text", "") for item in save_events
                                   if item.get("type") == "output"))
        raw_config = editable_config.read_text(encoding="utf-8")
        assert "// retain this explanation" in raw_config, raw_config
        assert '"UnknownSetting": { "preserve": true }' in raw_config, raw_config
        assert '"OutDirectory": "new-output"' in raw_config, raw_config
        assert Path(str(editable_config) + ".bak").exists()
        assert saved["revision"] != snapshot["revision"], saved

        stale_request = {
            **save_request,
            "requestId": "config-stale",
        }
        _, stale_completed = call_config(stale_request)
        assert stale_completed["status"] == "failed", stale_completed

        missing_config = root / "missing-config" / "config.json"
        missing_read = {
            "protocolVersion": 1, "requestId": "config-missing", "type": "run",
            "operation": "readConfiguration", "parameters": {"configPath": str(missing_config)},
        }
        missing_events, missing_completed = call_config(missing_read)
        assert missing_completed["status"] == "succeeded", missing_events
        missing_snapshot = json.loads("".join(item.get("text", "") for item in missing_events
                                              if item.get("type") == "output"))
        assert not missing_snapshot["exists"] and missing_snapshot["revision"] == "missing"
        assert not missing_config.exists()
        create_request = {
            "protocolVersion": 1, "requestId": "config-create", "type": "run",
            "operation": "saveConfiguration",
            "parameters": {
                "configPath": str(missing_config), "configRevision": "missing",
                "configurationPatch": json.dumps({"OutDirectory": str(root / "created-output")}),
            },
        }
        created_events, created_completed = call_config(create_request)
        assert created_completed["status"] == "succeeded", created_events
        assert json.loads(missing_config.read_text(encoding="utf-8"))["OutDirectory"] == \
            str(root / "created-output")

        default_config = root / "default-config" / "config.json"
        initialize_default = {
            "protocolVersion": 1, "requestId": "config-default-create", "type": "run",
            "operation": "initializeDefaultConfig",
            "parameters": {"configPath": str(default_config)},
        }
        default_events, default_completed = call_config(initialize_default)
        assert default_completed["status"] == "succeeded", default_events
        default_text = default_config.read_text(encoding="utf-8")
        assert '"MediaDirectoryList"' in default_text, default_text
        assert "// MetadataSidecars applies to conversion runs only" in default_text, default_text
        default_snapshot_request = {
            "protocolVersion": 1, "requestId": "config-default-read", "type": "run",
            "operation": "readConfiguration", "parameters": {"configPath": str(default_config)},
        }
        default_snapshot_events, default_snapshot_completed = call_config(default_snapshot_request)
        assert default_snapshot_completed["status"] == "succeeded", default_snapshot_events
        default_snapshot_text = "".join(item.get("text", "") for item in default_snapshot_events
                                        if item.get("type") == "output")
        default_snapshot = json.loads(default_snapshot_text)
        assert default_snapshot["configuration"]["OutDirectory"] == "", default_snapshot
        assert default_snapshot["configuration"]["MediaDirectoryList"] == [], default_snapshot
        original_default_text = default_config.read_text(encoding="utf-8")
        initialize_default["requestId"] = "config-default-refuse-overwrite"
        _, refused_default = call_config(initialize_default)
        assert refused_default["status"] == "failed", refused_default
        assert default_config.read_text(encoding="utf-8") == original_default_text

        per_user_config = root / "local-app-data" / "AudioLibraryToolkit" / "config.json"
        _, per_user_created = call_config({
            "protocolVersion": 1, "requestId": "config-default-per-user", "type": "run",
            "operation": "initializeDefaultConfig", "parameters": {},
        })
        assert per_user_created["status"] == "succeeded", per_user_created
        assert per_user_config.is_file(), per_user_config


if __name__ == "__main__":
    main()
