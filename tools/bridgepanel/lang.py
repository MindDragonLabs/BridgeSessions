"""Filename + first-line language detection (Notepad++ order, no ML)."""
from __future__ import annotations

import re
from pathlib import Path

# Special basenames beat the extension (Makefile, Dockerfile, …).
SPECIAL_NAMES: dict[str, str] = {
    "makefile": "makefile",
    "gnumakefile": "makefile",
    "cmakelists.txt": "cmake",
    "dockerfile": "dockerfile",
    "containerfile": "dockerfile",
    "rakefile": "ruby",
    "vagrantfile": "ruby",
    "gemfile": "ruby",
    "sconstruct": "python",
    "sconscript": "python",
    "wscript": "python",
    "crontab": "shell",
    ".bashrc": "shell",
    ".zshrc": "shell",
    ".profile": "shell",
    ".bash_profile": "shell",
}

EXT_TO_LANG: dict[str, str] = {
    ".py": "python",
    ".pyw": "python",
    ".pyi": "python",
    ".ps1": "powershell",
    ".psm1": "powershell",
    ".psd1": "powershell",
    ".js": "javascript",
    ".mjs": "javascript",
    ".cjs": "javascript",
    ".jsx": "javascript",
    ".ts": "typescript",
    ".tsx": "typescript",
    ".json": "json",
    ".jsonc": "json",
    ".html": "html",
    ".htm": "html",
    ".css": "css",
    ".scss": "css",
    ".xml": "xml",
    ".svg": "xml",
    ".yaml": "yaml",
    ".yml": "yaml",
    ".toml": "toml",
    ".ini": "ini",
    ".cfg": "ini",
    ".conf": "ini",
    ".env": "ini",
    ".sh": "shell",
    ".bash": "shell",
    ".zsh": "shell",
    ".ksh": "shell",
    ".c": "c",
    ".h": "c",
    ".cpp": "cpp",
    ".cc": "cpp",
    ".cxx": "cpp",
    ".hpp": "cpp",
    ".go": "go",
    ".rs": "rust",
    ".rb": "ruby",
    ".php": "php",
    ".sql": "sql",
    ".java": "java",
    ".kt": "java",
    ".cs": "csharp",
    ".lua": "lua",
    ".r": "r",
    ".pl": "perl",
    ".pm": "perl",
    ".bat": "batch",
    ".cmd": "batch",
    ".dockerfile": "dockerfile",
}

# First-line hints — only used when name/ext did not match.
_FIRST_LINE: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"^#!.*\bpython[0-9.]*\b", re.I), "python"),
    (re.compile(r"^#!.*\b(ba|z|k|fi)?sh\b", re.I), "shell"),
    (re.compile(r"^#!.*\b(pwsh|powershell)\b", re.I), "powershell"),
    (re.compile(r"^#!.*\b(node|nodejs)\b", re.I), "javascript"),
    (re.compile(r"^#!.*\bperl\b", re.I), "perl"),
    (re.compile(r"^#!.*\bruby\b", re.I), "ruby"),
    (re.compile(r"^#!.*\bphp\b", re.I), "php"),
    (re.compile(r"^<\?xml\b", re.I), "xml"),
    (re.compile(r"^<\?php\b", re.I), "php"),
    (re.compile(r"^<!DOCTYPE\s+html\b", re.I), "html"),
    (re.compile(r"^<html\b", re.I), "html"),
]

_LANG_LABELS: dict[str, str] = {
    "plaintext": "Plain Text",
    "python": "Python",
    "powershell": "PowerShell",
    "javascript": "JavaScript",
    "typescript": "TypeScript",
    "json": "JSON",
    "html": "HTML",
    "css": "CSS",
    "xml": "XML",
    "yaml": "YAML",
    "toml": "TOML",
    "ini": "INI",
    "shell": "Shell",
    "c": "C",
    "cpp": "C++",
    "go": "Go",
    "rust": "Rust",
    "ruby": "Ruby",
    "php": "PHP",
    "sql": "SQL",
    "java": "Java",
    "csharp": "C#",
    "lua": "Lua",
    "r": "R",
    "perl": "Perl",
    "batch": "Batch",
    "dockerfile": "Dockerfile",
    "makefile": "Makefile",
    "cmake": "CMake",
    "markdown": "Markdown",
}


def detect_language(name: str, first_line: str = "", override: str | None = None) -> str:
    """N++ order: override → special filename → extension → first line → plaintext."""
    if override:
        return override
    base = Path(name or "").name
    special = SPECIAL_NAMES.get(base.lower())
    if special:
        return special
    ext = Path(base).suffix.lower()
    if ext in EXT_TO_LANG:
        return EXT_TO_LANG[ext]
    line = (first_line or "").splitlines()[0] if first_line else ""
    if line:
        for rx, lang in _FIRST_LINE:
            if rx.search(line):
                return lang
    return "plaintext"


def is_code_name(name: str) -> bool:
    """True when the name maps to a highlighter (not md/media — caller checks those first)."""
    return detect_language(name) != "plaintext"


def languages() -> list[dict[str, str]]:
    """Picker rows: id + label, plaintext first then A–Z."""
    rows = [{"id": lid, "label": _LANG_LABELS.get(lid, lid)} for lid in _LANG_LABELS]
    rows.sort(key=lambda r: (r["id"] != "plaintext", r["label"].lower()))
    return rows


def js_table() -> str:
    """JSON for the panel: special names, extensions, first-line sources (as strings)."""
    import json

    first = [{"pat": rx.pattern, "flags": "i" if (rx.flags & re.I) else "", "lang": lang}
             for rx, lang in _FIRST_LINE]
    return json.dumps({
        "special": SPECIAL_NAMES,
        "ext": EXT_TO_LANG,
        "first": first,
        "languages": languages(),
    }, separators=(",", ":"))
