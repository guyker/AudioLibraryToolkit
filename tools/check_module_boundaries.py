#!/usr/bin/env python3
"""Check repository-owned module boundaries without joining either product's build."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


DESKTOP = "AudioLibraryToolkit.Desktop"
BACKEND_DEPENDENCY = re.compile(
    r"AudioLibraryToolkit\.Desktop|(?:^|[\s\"'/\\])ui[/\\]|"
    r"\belectron\b|\bnode_modules\b|\bnpm\b|\bnpx\b",
    re.IGNORECASE,
)
FRONTEND_DETAIL = re.compile(
    r"AudioLibraryToolkit\.Desktop|\belectron\b|\btypescript\b|\breact\b|\brenderer\b|`ui[/\\]",
    re.IGNORECASE,
)
PRIVATE_BACKEND = re.compile(r"AudioLibraryToolkit[/\\](?:src|include)|\bcmake\b", re.IGNORECASE)
RENDERER_IMPORT = re.compile(
    r"""(?:from\s*|import\s*(?:\(\s*)?|require\s*\(\s*)['"](?:electron|react)(?:/|['"])""",
)
MARKDOWN_LINK = re.compile(r"\[[^\]\n]+\]\(([^)\s]+)(?:\s+[^)]*)?\)")
HISTORICAL_KNOWLEDGE = re.compile(r"\b(?:superseded|historical|archived|archive|fixed|retired)\b", re.IGNORECASE)


def backend_errors(path: str, text: str, *, documentation: bool = False) -> list[str]:
    forbidden = FRONTEND_DETAIL if documentation else BACKEND_DEPENDENCY
    return [f"{path}: frontend dependency/detail {match.group(0)!r}" for match in forbidden.finditer(text)]


def client_errors(path: str, text: str, *, sdk: bool = False) -> list[str]:
    errors = [f"{path}: private backend/build reference {m.group(0)!r}" for m in PRIVATE_BACKEND.finditer(text)]
    if sdk:
        errors.extend(f"{path}: worker client imports renderer/host API" for _ in RENDERER_IMPORT.finditer(text))
    return errors


def link_errors(path: Path) -> list[str]:
    errors = []
    for match in MARKDOWN_LINK.finditer(path.read_text(encoding="utf-8-sig")):
        target = match.group(1).split("#", 1)[0]
        if not target or re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", target) or "<" in target:
            continue
        if not (path.parent / target).exists():
            errors.append(f"{path}: missing link target {target}")
    return errors


def module_link_errors(path: Path, module: Path) -> list[str]:
    errors = []
    module_root = module.resolve()
    for match in MARKDOWN_LINK.finditer(path.read_text(encoding="utf-8-sig")):
        target = match.group(1).split("#", 1)[0]
        if not target or re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", target) or "<" in target:
            continue
        resolved = (path.parent / target).resolve()
        try:
            resolved.relative_to(module_root)
        except ValueError:
            errors.append(f"{path}: module-owned documentation links outside {module.name}: {target}")
    return errors


def inactive_knowledge_link_errors(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8-sig")
    source_status = re.search(r"(?m)^status:\s*([^\s#]+)", text)
    if source_status and source_status.group(1).lower() != "active":
        return []
    errors = []
    for match in MARKDOWN_LINK.finditer(text):
        target = match.group(1).split("#", 1)[0]
        if not target or re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", target) or "<" in target:
            continue
        resolved = (path.parent / target).resolve()
        if "knowledge" not in {part.lower() for part in resolved.parts} or not resolved.is_file():
            continue
        target_text = resolved.read_text(encoding="utf-8-sig")
        target_status = re.search(r"(?m)^status:\s*([^\s#]+)", target_text)
        if not target_status or target_status.group(1).lower() == "active":
            continue
        line_start = text.rfind("\n", 0, match.start()) + 1
        line_end = text.find("\n", match.end())
        line = text[line_start:] if line_end < 0 else text[line_start:line_end]
        if not HISTORICAL_KNOWLEDGE.search(line):
            errors.append(
                f"{path}: knowledge link to non-active entry needs an explicit historical label: {target}"
            )
    return errors


def check(root: Path) -> list[str]:
    errors: list[str] = []
    backend_inputs = [root / "CMakeLists.txt", root / "CMakePresets.json"]
    backend_inputs.extend((root / "cmake").rglob("*.cmake"))
    backend_inputs.extend((root / "AudioLibraryToolkit").rglob("CMakeLists.txt"))
    for area in ("src", "include"):
        backend_inputs.extend(
            p for p in (root / "AudioLibraryToolkit" / area).rglob("*")
            if p.suffix in {".cpp", ".h", ".hpp"} and "rapidjson" not in p.parts
        )
    backend_inputs.extend(p for p in (root / "tests").rglob("*") if p.suffix in {".py", ".cpp", ".txt"})
    for path in backend_inputs:
        errors.extend(backend_errors(str(path.relative_to(root)), path.read_text(encoding="utf-8-sig")))

    backend_docs = [root / "AudioLibraryToolkit" / name for name in ("README.md", "DESIGN.md")]
    backend_docs.extend(p for p in (root / "docs").rglob("*.md") if "archive" not in p.parts)
    for path in backend_docs:
        errors.extend(backend_errors(str(path.relative_to(root)), path.read_text(encoding="utf-8-sig"),
                                     documentation=True))

    module = root / DESKTOP
    for name in ("README.md", "DESIGN.md", "AGENTS.md", "docs/features/index.md",
                 "docs/knowledge/README.md", "docs/knowledge/kb.config.json", "docs/knowledge/index.md"):
        if not (module / name).is_file():
            errors.append(f"{DESKTOP}: missing module-owned {name}")
    for area in ("apps/desktop/desktop", "apps/desktop/renderer/src", "apps/desktop/test",
                 "packages/worker-client/src", "packages/worker-client/test"):
        for path in (module / area).rglob("*"):
            if path.suffix in {".mjs", ".cjs", ".ts", ".tsx"}:
                errors.extend(client_errors(str(path.relative_to(root)), path.read_text(encoding="utf-8-sig"),
                                            sdk=area.startswith("packages/")))
    for path in module.rglob("package.json"):
        if "node_modules" not in path.parts and "build" not in path.parts:
            errors.extend(client_errors(str(path.relative_to(root)), path.read_text(encoding="utf-8-sig")))
    desktop_docs = [module / "README.md", module / "DESIGN.md", module / "AGENTS.md"]
    desktop_docs.extend(p for p in (module / "docs").rglob("*.md") if "archive" not in p.parts)
    repository_docs = [root / "README.md", *backend_docs, *desktop_docs]
    for path in repository_docs:
        if path.is_file():
            errors.extend(link_errors(path))
            errors.extend(inactive_knowledge_link_errors(path))
    for path in desktop_docs:
        if path.is_file():
            errors.extend(module_link_errors(path, module))
    for path in (module / "docs/knowledge").glob("UI-K-*.md"):
        text = path.read_text(encoding="utf-8-sig")
        for citation in re.findall(r"^\s+- path:\s*(.+)$", text, re.MULTILINE):
            cited = Path(citation.strip("\"'"))
            if cited.is_absolute() or ".." in cited.parts or not (module / cited).is_file():
                errors.append(f"{path}: citation is not a module-owned file: {citation}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root.resolve())
    for error in errors:
        print(error)
    if errors:
        print(f"Module boundaries: {len(errors)} error(s).")
        return 1
    print("Module dependencies, documentation ownership and desktop links are valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
