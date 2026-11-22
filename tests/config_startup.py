"""Exercise startup configuration without touching a user's settings or libraries."""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def load_config(path):
    """Parse a configuration copied from the commented template; comments are whole lines."""
    text = path.read_text(encoding="utf-8")
    assert "//" in text, "First run did not copy the commented default template"
    return json.loads("\n".join(line for line in text.splitlines() if not line.lstrip().startswith("//")))


def main():
    converter = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="alt-config-") as directory:
        root = Path(directory)
        env = os.environ.copy()
        if os.name != "nt":
            env["XDG_STATE_HOME"] = str(root / "state-home")
        if os.name == "nt":
            env["LOCALAPPDATA"] = str(root / "local")
            config = Path(env["LOCALAPPDATA"]) / "AudioLibraryToolkit" / "config.json"
        elif sys.platform == "darwin":
            env["HOME"] = str(root / "home")
            config = Path(env["HOME"]) / "Library" / "Application Support" / "AudioLibraryToolkit" / "config.json"
        else:
            env["XDG_CONFIG_HOME"] = str(root / "config-home")
            config = Path(env["XDG_CONFIG_HOME"]) / "AudioLibraryToolkit" / "config.json"

        def run(*args, stdin=""):
            return subprocess.run(
                [str(converter), *map(str, args)], input=stdin, env=env, cwd=root,
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)

        for option in ("--help", "--version"):
            result = run(option)
            assert result.returncode == 0, result.stdout + result.stderr
            assert str(config) in result.stdout, result.stdout
            assert not config.exists(), "Informational commands created a configuration"

        result = run()
        text = result.stdout + result.stderr
        assert result.returncode != 0, text
        assert str(config) in text and "Open and edit" in text, text
        settings = load_config(config)
        assert settings["OutDirectory"] == "" and settings["MediaDirectoryList"] == [], settings

        if os.name != "nt":
            import pty

            redirected_env = env.copy()
            redirected_env["HOME"] = str(root / "redirected-home")
            redirected_env["XDG_CONFIG_HOME"] = str(root / "redirected-config")
            if sys.platform == "darwin":
                redirected_config = Path(redirected_env["HOME"]) / "Library" / "Application Support" / "AudioLibraryToolkit" / "config.json"
            else:
                redirected_config = Path(redirected_env["XDG_CONFIG_HOME"]) / "AudioLibraryToolkit" / "config.json"
            master, slave = pty.openpty()
            try:
                result = subprocess.run(
                    [str(converter)], stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    env=redirected_env, cwd=root, text=True, encoding="utf-8", timeout=10)
            finally:
                os.close(master)
                os.close(slave)
            assert result.returncode != 0 and "Open and edit" in result.stdout + result.stderr
            redirected_settings = load_config(redirected_config)
            assert redirected_settings["OutDirectory"] == "" and redirected_settings["MediaDirectoryList"] == []

        library = root / "input \u97f3"
        library.mkdir()
        destination = root / "output \u00e9"
        if os.name != "nt":
            interactive_env = env.copy()
            interactive_env["HOME"] = str(root / "interactive-home")
            interactive_env["XDG_CONFIG_HOME"] = str(root / "interactive-config")
            if sys.platform == "darwin":
                interactive_config = Path(interactive_env["HOME"]) / "Library" / "Application Support" / "AudioLibraryToolkit" / "config.json"
            else:
                interactive_config = Path(interactive_env["XDG_CONFIG_HOME"]) / "AudioLibraryToolkit" / "config.json"
            master, slave = pty.openpty()
            process = None
            try:
                process = subprocess.Popen(
                    [str(converter)], stdin=slave, stdout=slave, stderr=slave,
                    env=interactive_env, cwd=root)
                os.close(slave)
                slave = None
                os.write(master, f"{destination.name}\n{library.name}\n0\n".encode("utf-8"))
                returncode = process.wait(timeout=10)
                terminal_output = os.read(master, 65536).decode("utf-8", errors="replace")
            finally:
                os.close(master)
                if slave is not None:
                    os.close(slave)
                if process is not None and process.poll() is None:
                    process.wait(timeout=10)
            assert returncode == 0, terminal_output
            interactive_settings = load_config(interactive_config)
            assert interactive_settings["OutDirectory"] == str(destination)
            assert interactive_settings["MediaDirectoryList"][0]["mediaPath"] == str(library)

        settings["OutDirectory"] = str(destination)
        config.write_text(json.dumps(settings), encoding="utf-8")
        before = config.read_bytes()
        result = run(stdin="0\n")
        assert result.returncode != 0 and "at least one active" in result.stdout + result.stderr
        assert config.read_bytes() == before, "Incomplete user configuration was modified"

        settings["MediaDirectoryList"] = [{"isActive": True, "mediaPath": str(library)}]
        config.write_text(json.dumps(settings), encoding="utf-8")
        result = run(stdin="0\n")
        assert result.returncode == 0, result.stdout + result.stderr
        assert str(config) in result.stdout and "open and edit" in result.stdout, result.stdout
        assert "Loaded 1 active media libraries" in result.stdout, result.stdout

        custom = root / "custom.json"
        custom.write_text("{}", encoding="utf-8")
        result = run("repair", library, "--dry-run", "--config", custom)
        assert result.returncode != 0 and "Set OutDirectory" in result.stdout + result.stderr
        result = run("repair", library, "--dry-run", "--config", custom, "--output-dir", " ")
        assert result.returncode != 0 and "output directory must not be blank" in result.stdout + result.stderr
        result = run("repair", library, "--dry-run", "--output-dir", destination, f"--config={custom}")
        assert result.returncode == 0, result.stdout + result.stderr
        assert str(custom) in result.stdout, result.stdout
        result = run("repair", library, "--dry-run", "--output-dir", destination, "--config", custom.name)
        assert result.returncode == 0 and str(custom) in result.stdout, result.stdout + result.stderr

        missing = root / "missing.json"
        result = run("scan", library, "--output-dir", destination, "--config", missing)
        assert result.returncode != 0 and str(missing) in result.stdout + result.stderr
        assert not missing.exists(), "An explicit missing config was created"


if __name__ == "__main__":
    main()
