#!/usr/bin/env python3
"""Structural smoke test for AIagent98 tools.c (no Win9x runtime required).

IMPORTANT: this file is a *source/documentation presence* check.  A needle
surviving in tools.c does NOT prove the corresponding safety behaviour works.
The negative/behavioural security assertions live in tests/test_tools.c
(banned shell commands, sandbox escape, confirm callback, auto_confirm) and in
tests/gui_smoke.ps1 (the HIGH RISK full-YES prompt).  Keep the two in sync.
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
tools_c = root / "src" / "agent" / "tools.c"
text = tools_c.read_text(encoding="utf-8", errors="replace")

# The authoritative list is g_tools[] in tools.c.  It currently holds 46
# entries; this list is kept here so an accidental removal or rename is caught,
# not just a wrong count.  Update it together with g_tools[] and the docs.
expected = [
    "list_dir", "read_file", "write_file", "search_files", "run_command",
    "mkdir", "delete_file", "rmdir", "rmdir_force", "move_file", "copy_file",
    "append_file", "edit", "apply_patch", "touch", "file_info", "set_attr",
    "tree", "wc", "which", "glob", "grep", "head", "tail", "diff", "checksum",
    "disk_info", "env", "session_info", "todo_read", "todo_write",
    "build", "replace_across", "outline", "restore", "snapshot", "hexdump",
    "http_request", "http_download", "dns_resolve", "tcp_probe", "web_search",
    "gist_upload", "list_skills", "use_skill", "save_skill",
]
expected_count = 46

# Scan only the g_tools[] array: a `{"name", ...}` literal elsewhere in the
# file must not be counted as a registered tool.
start = text.find("static const ToolDef g_tools[] = {")
end = text.find("\n};", start) if start >= 0 else -1
block = text[start:end] if start >= 0 and end > start else text
names = re.findall(r'\{\s*"([a-z_][a-z_0-9]*)"\s*,', block)
found = [n for n in names if n in expected]
missing = [n for n in expected if n not in found]

fail = 0
print(f"tools registered: {len(found)} / {len(expected)}")
if len(expected) != expected_count:
    print(f"FAIL: expected list has {len(expected)} names, docs say "
          f"{expected_count}")
    fail = 1
if len(found) != len(names) or len(names) != expected_count:
    print(f"FAIL: g_tools[] has {len(names)} entries, expected {expected_count}")
    fail = 1
if missing:
    print("MISSING:", ", ".join(missing))
    fail = 1

# Keep documentation honest: the g_tools[] count must match the number stated
# in the two reference documents.
for doc in (root / "README.md", root / "TOOLS.md"):
    doc_text = doc.read_text(encoding="utf-8", errors="replace") if doc.exists() else ""
    if not re.search(r"\b46\b", doc_text):
        print(f"FAIL: {doc.name} does not state the 46-tool count")
        fail = 1
    if re.search(r"\b47\b|\b43\b", doc_text):
        print(f"FAIL: {doc.name} still claims 47 or 43 tools")
        fail = 1

# Source-presence checks only (NOT safety assertions); see module docstring.
presence_checks = {
    "grep_walk": "grep recursive walker",
    "redact_secrets": "secret redaction",
    "blocked dangerous command": "run_command ban list",
    ".bak": "apply_patch backup",
    "overwrite": "copy_file overwrite",
    '"cwd"': "run_command cwd",
}

for needle, label in presence_checks.items():
    if needle not in text:
        print(f"FAIL (presence only): {label} ({needle})")
        fail = 1
    else:
        print(f"OK (presence only): {label}")

depth = 0
i = 0
n = len(text)
state = "code"
while i < n:
    c = text[i]
    nxt = text[i + 1] if i + 1 < n else ""
    if state == "code":
        # Comments must be recognised before quotes, otherwise an apostrophe
        # or quote inside a comment flips the scanner into string mode and
        # every brace after it is mis-counted.
        if c == "/" and nxt == "/":
            state = "line"
            i += 2
            continue
        if c == "/" and nxt == "*":
            state = "block"
            i += 2
            continue
        if c == '"':
            state = "str"
            i += 1
            continue
        if c == "'":
            state = "chr"
            i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    elif state == "line":
        if c == "\n":
            state = "code"
        i += 1
    elif state == "block":
        if c == "*" and nxt == "/":
            state = "code"
            i += 2
        else:
            i += 1
    elif state == "str":
        if c == "\\":
            i += 2
        elif c == '"':
            state = "code"
            i += 1
        else:
            i += 1
    else:  # chr literal
        if c == "\\":
            i += 2
        elif c == "'":
            state = "code"
            i += 1
        else:
            i += 1
if depth != 0:
    print(f"FAIL: brace depth {depth}")
    fail = 1
else:
    print("OK: brace balance")

sys.exit(fail)
