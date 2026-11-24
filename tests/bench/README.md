# a98 benchmark harness

Runs the real `a98.exe` against an OpenAI Chat Completions-compatible bridge
and scores the resulting workspace with a 100-point rubric.

## Usage

From the repository root (PowerShell, `-NoProfile` compatible):

```powershell
# Deterministic mock model, no API key or OpenCode needed
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bench\run_bench.ps1 -Mock

# Pick a task
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bench\run_bench.ps1 -Task bugfix -Mock
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bench\run_bench.ps1 -Task multifile -Mock

# Live model via the OpenCode CLI
powershell -NoProfile -ExecutionPolicy Bypass -File tests\bench\run_bench.ps1 -Task csv_parse
```

Available tasks: `csv_parse` (default), `bugfix`, `multifile`. Each has its own
`task.txt`, one-line prompt, workspace fixture, hidden tests and scoring hook
under `tasks\<task>\`.

`run_bench.ps1` resets `ws\` and `hidden\` from the selected task fixture,
regenerates `run\agent98.ini` with the correct workspace path and port, starts
the bridge, runs the agent with the one-line task text, invokes `score.ps1`,
prints the result and stops the bridge. The agent transcript is saved to
`agent_output.txt`, and bridge traffic to `proxy.log` (both under this folder).

The bridge can also be started by hand:

```powershell
node tests\bench\bench_proxy.js --mock=csv_parse   # or --mock=bugfix / multifile
set BENCH_MOCK=1 && node tests\bench\bench_proxy.js  # default csv_parse task
node tests\bench\bench_proxy.js                    # live mode
```

Use `--port=8802` (or `BENCH_PORT`) to change the listener; `run_bench.ps1`
passes its `-Port` through automatically.

## Mock vs live

* **Mock** returns a canned, correct tool sequence: read the fixture files,
  write the fixed source(s), run `run_tests.bat`, then `ANSWER:`. It needs only
  Node.js v22+ and `gcc` on PATH, and produces a stable score of 100/100 when
  the harness itself is intact. It lives in `bench_proxy.js` next to the live
  bridge and follows the same non-streaming `tool_calls` shape as
  `tests/mock_openai.js`.
* **Live** calls `opencode api post /api/experimental/generate/text` with the
  repository root as the working location, exactly like the original harness.
  It requires the OpenCode CLI (`BENCH_OPENCODE` can override its path)
  configured with a working model and valid credentials. `agent98.ini` points
  at `http://127.0.0.1:8801/v1`. Both streaming and non-streaming responses
  are supported.

## Scoring (`score.ps1`)

| Component | Points | What it checks |
|-----------|-------:|----------------|
| Compile   | 25     | Hidden test compiles against the workspace sources |
| Visible   | 20     | `tests/test_visible.c` builds and prints `all visible tests passed` |
| Hidden    | 40     | Hidden test's `HIDDEN SCORE: n/total` scaled to 40 |
| Efficiency| 10     | 10 for <= 10 model rounds, 6 for <= 12, 0 otherwise (counted from `proxy.log`) |
| Integrity | 5      | Provided headers unchanged and required files still present |

Run just the scorer with `powershell -NoProfile -File tests\bench\score.ps1 -Task csv_parse`.
