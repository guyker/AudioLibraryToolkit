"""Verify the machine-readable worker boundary using temporary inputs."""

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def test_repair_selection(executable, root, env):
    source = root / "selection"
    source.mkdir()
    (source / "notes.txt").write_bytes(b"album companion")
    output = root / "selection-output"
    configured = root / "selection.json"
    configured.write_text(json.dumps({
        "OutDirectory": str(output), "RepairCategories": {"integrity": "detect"},
        "MediaDirectoryList": [{"mediaPath": str(source)}],
    }), encoding="utf-8")
    defaults = {"metadata": "fix", "integrity": "off", "container": "off", "decode": "off"}
    def snapshot():
        return {p.relative_to(source): p.read_bytes() if p.is_file() else None for p in source.rglob("*")}

    before = snapshot()

    def request(name, kind="run", **parameters):
        return json.dumps({
            "protocolVersion": 1, "requestId": name, "type": kind, "operation": "repair",
            "parameters": {"inputPath": str(source), "configPath": str(configured),
                           "dryRun": True, **parameters},
        })

    malformed = [[], None, {"stream": "detect"}, {"metadata": False}, {"decode": "fix"},
                 {"container": "fix"}, {"integrity": "fix\0no"}]
    invalid_requests = {f"bad-category-{i}": value for i, value in enumerate(malformed)}
    legacy = {"repairCategories": defaults, "rules": []}
    lines = [request(name, repairCategories=value) for name, value in invalid_requests.items()]
    lines.extend([
        request("duplicate-mode", repairCategories={"metadata": "fix"}).replace(
            '"metadata": "fix"', '"metadata":"fix","metadata":"off"'),
        request("unknown-rule", rules=["flac.not-a-rule"]),
        request("legacy", **legacy),
        request("legacy-inherited", configPath=str(root / "config.json"), outputDirectory=str(output)),
        request("configured", rules=[]),
        request("override", repairCategories={"metadata": "off"}, rules=["flac.trailing-data"]),
        request("preview", "preview", repairCategories={"metadata": "off"}, rules=["flac.trailing-data"]),
        request("id3-union", repairCategories=defaults, rules=["mp3.text-encoding"], removeId3v1=True),
        request("id3-detect", repairCategories={"metadata": "detect"}, rules=["mp3.redundant-id3v1"]),
        request("id3-conflict", repairCategories={"metadata": "detect"}, removeId3v1=True, dryRun=False),
        request("disabled-rule", repairCategories=defaults, rules=["flac.trailing-data"], dryRun=False),
        request("all-off", repairCategories={"integrity": "off", "metadata": "off"}, dryRun=False),
        request("protected-library", inputPath=str(root / "input"), outputDirectory=str(source), dryRun=False),
    ])
    result = subprocess.run([str(executable), "--worker"], input="\n".join(lines) + "\n", env=env,
                            capture_output=True, text=True, encoding="utf-8", timeout=60)
    assert result.returncode == 0, result.stderr
    events = [json.loads(line) for line in result.stdout.splitlines()]
    assert "repair.categories.v1" in events[0]["features"] and "repair" in events[0]["capabilities"]

    def replies(name):
        return [e for e in events if e["requestId"] == name]

    def text(name):
        return "".join(e["text"] for e in replies(name) if e["type"] == "output")

    def report(name):
        assert replies(name)[-1].get("exitCode") == 0, (replies(name), result.stderr)
        return json.loads(text(name))

    for name in (*invalid_requests, "duplicate-mode", "unknown-rule"):
        assert [e["type"] for e in replies(name)] == ["error"], (name, replies(name))
    for name in ("id3-conflict", "disabled-rule", "all-off", "protected-library"):
        reply = replies(name)[-1]
        assert reply["type"] == "error" or reply.get("status") == "failed", (name, reply)
    legacy_report = report("legacy")
    assert legacy_report == report("legacy-inherited") and legacy_report["schemaVersion"] == 2
    assert "categories" not in legacy_report and "flac.trailing-data" not in legacy_report["rules"]
    assert "mp3.redundant-id3v1" not in legacy_report["rules"]
    assert report("configured")["categories"] == {**defaults, "integrity": "detect"}
    selected = report("override")
    assert selected["schemaVersion"] == 3 and selected["rules"] == ["flac.trailing-data"], selected
    assert selected["categories"] == {**defaults, "metadata": "off", "integrity": "detect"}
    assert replies("preview")[-1].get("exitCode") == 0 and "PCM" in text("preview")
    assert "integrity=detect" in text("preview") and "Metadata=off" in text("preview")
    assert set(report("id3-union")["rules"]) == {"mp3.text-encoding", "mp3.redundant-id3v1"}
    assert report("id3-detect")["rules"] == ["mp3.redundant-id3v1"]
    assert snapshot() == before and not output.exists()

    cli = [str(executable), "repair", str(source), "--config", str(configured)]
    dry_run = subprocess.run([*cli, "--integrity-mode", "off", "--dry-run"], env=env,
                             capture_output=True, text=True, encoding="utf-8", timeout=60)
    assert dry_run.returncode == 0 and json.loads(dry_run.stdout) == legacy_report, dry_run
    config_only = subprocess.run([*cli, "--dry-run"], env=env, capture_output=True,
                                 text=True, encoding="utf-8", timeout=60)
    assert config_only.returncode == 0 and json.loads(config_only.stdout) == report("configured"), config_only
    assert "Repair discovery:" in config_only.stderr
    old_cli = subprocess.run([str(executable), "repair", str(source), "--config", str(root / "config.json"),
                              "--output-dir", str(output), "--dry-run"], env=env, capture_output=True,
                             text=True, encoding="utf-8", timeout=60)
    assert old_cli.returncode == 0 and "Repair discovery:" in old_cli.stdout
    assert [json.loads(line) for line in old_cli.stdout.splitlines() if line.startswith("{")] == [legacy_report]
    for flag in ("--container-mode", "--decode-mode"):
        refused = subprocess.run([*cli, flag, "fix"], env=env, capture_output=True, timeout=60)
        assert refused.returncode != 0 and not output.exists(), refused
    assert snapshot() == before

    # Keep both the per-user menu configuration and its retained HTML previews in the fixture.
    home = root / "menu-home"
    preview_dir = root / "menu-previews"
    preview_dir.mkdir()
    menu_env = dict(env, LOCALAPPDATA=str(home), XDG_CONFIG_HOME=str(home), HOME=str(home),
                    TEMP=str(preview_dir), TMP=str(preview_dir), TMPDIR=str(preview_dir))
    config_dir = home / "Library" / "Application Support" if sys.platform == "darwin" else home
    config_dir = config_dir / "AudioLibraryToolkit"
    config_dir.mkdir(parents=True)
    (config_dir / "config.json").write_bytes(configured.read_bytes())

    def menu(answers):
        run = subprocess.run([str(executable)], input=answers, env=menu_env,
                             capture_output=True, text=True, encoding="utf-8", timeout=60)
        assert run.returncode == 0, (run.stdout, run.stderr)
        return run.stdout

    for stop in ("BACK\n0\n", "EXIT\n", ""):
        cancelled = menu("7\nCATEGORIES\noff\n" + stop)
        assert "Completion summary" not in cancelled and not output.exists()
    help_text = menu("?\n0\n")
    assert "integrity=detect" in help_text and "decoded PCM" in help_text
    completed = menu("7\nCATEGORIES\noff\n\noops\ndetect\noff\n\n\nRUN\n")
    assert "Invalid container mode" in completed and "ID3v1 removal is unavailable" in completed
    assert "Metadata=off" in completed.split("Completion summary:")[1]
    installed = json.loads((output / "repaired-metadata" / source.name / "repair-report.json").read_bytes())
    assert installed["categories"] == {**defaults, "metadata": "off", "integrity": "detect", "container": "detect"}
    assert snapshot() == before


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
        env["XDG_CONFIG_HOME"] = str(root / "local-app-data")
        env["XDG_STATE_HOME"] = str(root / "state")
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
        test_repair_selection(executable, root, env)

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

        def read_snapshot(path, request_id):
            snapshot_events, snapshot_completed = call_config({
                "protocolVersion": 1, "requestId": request_id, "type": "run",
                "operation": "readConfiguration", "parameters": {"configPath": str(path)},
            })
            assert snapshot_completed["status"] == "succeeded", snapshot_events
            return json.loads("".join(item.get("text", "") for item in snapshot_events
                                      if item.get("type") == "output"))

        def save_patch(path, revision, patch, request_id):
            return call_config({
                "protocolVersion": 1, "requestId": request_id, "type": "run",
                "operation": "saveConfiguration",
                "parameters": {"configPath": str(path), "configRevision": revision,
                               "configurationPatch": json.dumps(patch)},
            })

        # A nested object in a patch is merged member by member: comments and unknown keys inside the
        # existing object survive, and members the patch does not name keep their file values.
        nested_config = root / "nested.json"
        nested_config.write_text(
            '{\n  "OutDirectory": "old-output",\n'
            '  "FLACSettings": {\n'
            '    "ffmpeg_exe_name": "ffmpeg", // keep this note\n'
            '    "UnknownFlacKey": [1, 2]\n'
            '  },\n'
            '  "RepairCategories": {\n'
            '    "integrity": "detect" // retain structural checks\n'
            '  },\n'
            '  "MetadataSidecars": {\n'
            '    // nested explanation\n'
            '    "Format": "binary",\n'
            '    "UnknownNested": { "keep": true } // trailing note\n'
            '  }\n}\n',
            encoding="utf-8")
        nested_snapshot = read_snapshot(nested_config, "nested-read")
        custom_encoder = str(root / "ffmpeg-custom")
        nested_events, nested_saved = save_patch(nested_config, nested_snapshot["revision"], {
            "FLACSettings": {"ffmpeg_exe_name": custom_encoder, "ffmpeg_get_metadata_tags": "-hide_banner"},
            "MetadataSidecars": {"Format": "json", "ConvertToFlac": "optional", "ConvertToMp3": "disabled"},
            "RepairCategories": {"metadata": "off", "decode": "detect"},
        }, "nested-save")
        assert nested_saved["status"] == "succeeded", nested_events
        nested_raw = nested_config.read_text(encoding="utf-8")
        for kept in ("// keep this note", '"UnknownFlacKey": [1, 2]', "// nested explanation",
                     '"UnknownNested": { "keep": true }, // trailing note'):
            assert kept in nested_raw, nested_raw
        assert not re.search(r"\n\s*,", nested_raw), nested_raw
        assert nested_raw.endswith("}\n") and not nested_raw.endswith("\n\n"), repr(nested_raw)
        nested_after = read_snapshot(nested_config, "nested-read-after")
        assert nested_after["configuration"]["MetadataSidecars"] == {
            "Format": "json", "ConvertToFlac": "optional", "ConvertToMp3": "disabled"}, nested_after
        assert nested_after["configuration"]["FLACSettings"]["ffmpeg_exe_name"] == custom_encoder, nested_after
        assert nested_after["configuration"]["FLACSettings"]["ffmpeg_get_metadata_tags"] == "-hide_banner"
        assert "// retain structural checks" in nested_raw
        assert nested_after["configuration"]["RepairCategories"] == {
            "metadata": "off", "integrity": "detect", "container": "off", "decode": "detect"}

        _, partial_saved = save_patch(nested_config, nested_after["revision"],
                                      {"MetadataSidecars": {"Format": "binary"}}, "nested-partial")
        assert partial_saved["status"] == "succeeded", partial_saved
        partial_after = read_snapshot(nested_config, "nested-partial-after")
        assert partial_after["configuration"]["MetadataSidecars"] == {
            "Format": "binary", "ConvertToFlac": "optional", "ConvertToMp3": "disabled"}, partial_after
        partial_raw = nested_config.read_text(encoding="utf-8")
        assert "// nested explanation" in partial_raw
        assert partial_raw.endswith("}\n") and not partial_raw.endswith("\n\n"), repr(partial_raw)

        before_invalid = nested_config.read_text(encoding="utf-8")
        _, nested_invalid = save_patch(nested_config, partial_after["revision"],
                                       {"MetadataSidecars": {"Format": "xml"}}, "nested-invalid")
        assert nested_invalid["status"] == "failed", nested_invalid
        assert nested_config.read_text(encoding="utf-8") == before_invalid
        _, bad_categories = save_patch(nested_config, partial_after["revision"],
                                       {"RepairCategories": {"decode": "fix"}}, "bad-category-patch")
        assert bad_categories["status"] == "failed" and nested_config.read_text(encoding="utf-8") == before_invalid

        # Members and objects missing from the file are added with the file's own layout.
        sparse_config = root / "sparse.json"
        sparse_config.write_text('{ "OutDirectory": "old-output" }\n', encoding="utf-8")
        sparse_snapshot = read_snapshot(sparse_config, "sparse-read")
        _, sparse_saved = save_patch(sparse_config, sparse_snapshot["revision"],
                                     {"LogLevel": 2, "MetadataSidecars": {"Format": "json"}}, "sparse-save")
        assert sparse_saved["status"] == "succeeded", sparse_saved
        sparse_after = read_snapshot(sparse_config, "sparse-read-after")
        assert sparse_after["configuration"]["LogLevel"] == 2, sparse_after
        assert sparse_after["configuration"]["MetadataSidecars"]["Format"] == "json", sparse_after
        assert sparse_config.read_text(encoding="utf-8") == (
            '{ "OutDirectory": "old-output",\n  "LogLevel": 2,\n  "MetadataSidecars": {"Format":"json"}\n}\n')

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
