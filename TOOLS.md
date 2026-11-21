# AIagent98 Tools Reference (43)

## Read / Search
| Tool | Confirm | Notes |
|------|---------|-------|
| list_dir | no | List workspace directory |
| read_file | no | Optional start_line / max_lines |
| head / tail | no | First/last N lines (default 20) |
| file_info | no | Size, mtime, attributes |
| search_files | no | Literal content search |
| grep | no | Literal search or `regex=true` (`.` `*` `+` `?` `^` `$`); `context=N` adds surrounding lines; `include=*.c` filters basename |
| glob | no | `*` / `?` path patterns |
| tree | no | Directory tree, max_depth |
| wc | no | lines / words / bytes |
| which | no | Resolve name on PATH |
| diff | no | Line-by-line a vs b |
| checksum | no | CRC32 + size |

## Write / Change
| Tool | Confirm | Notes |
|------|---------|-------|
| write_file | yes | Full rewrite, ANSI+CRLF for text |
| append_file | yes | Append text |
| edit | yes | Unique old_string replace; replace_all option |
| apply_patch | yes | Multi-hunk `*** SEARCH` / `*** REPLACE` / `*** END`; writes `.bak` (aborts if the backup fails) |
| touch | no | Create empty or bump mtime |
| mkdir | no | Create parents |
| set_attr | yes | readonly / hidden / archive / system |

## Manage
| Tool | Confirm | Notes |
|------|---------|-------|
| delete_file | yes | Files only |
| rmdir | yes | Empty directory only |
| rmdir_force | yes | Needs `confirm=true`; recursive; never workspace root |
| move_file | yes | Rename/move file |
| copy_file | yes | `overwrite=true` optional |

## Execute / Env / Tasks
| Tool | Confirm | Notes |
|------|---------|-------|
| run_command | yes | Optional `cwd`; blocks format/deltree/rd /s etc. |
| disk_info | no | Drive free/total |
| env | yes | One var or common list; prompts before reading |
| session_info | no | OS, workspace, tool names |
| todo_read / todo_write | write yes | `.agent98_todos.txt` in workspace |

## Coding tools (extended)
| Tool | Confirm | Notes |
|------|---------|-------|
| build | yes | Run `build_ow.cmd` (or `command=`) and summarise compiler errors/warnings; needs `allow_shell=1` |
| replace_across | apply only | Literal search/replace over many files; `dry_run=true` by default; `include=*.c`, `max_files` |
| outline | no | List function and `#define` definitions in a source file |
| restore | yes | Restore a file from its `.bak` backup |
| snapshot | no | Copy the whole workspace to `<workspace>\.snapshots\<timestamp>[_name]\` |
| hexdump | no | Offset / hex / ASCII dump of a binary file (`offset`, `length`) |

## Network tools (extended, all gated by `[net] allow=1`)
| Tool | Confirm | Notes |
|------|---------|-------|
| http_request | GET/HEAD no, others yes | URL, `method`, `headers` (one `Name: value` per line), `body`, `out_file`, `max_bytes` |
| http_download | no | GET a URL to a workspace file; `max_bytes` |
| dns_resolve | no | Resolve a hostname to IPs |
| tcp_probe | no | Test `host:port` reachability (`timeout_sec`) |
| web_search | no | Search via `[net] search_url` (default DuckDuckGo HTML); `max_results` |
| gist_upload | yes | Upload a workspace file to a GitHub Gist; needs `[net] github_token` |

## Dangerous option: full system access
`[agent] full_access=1` (checkbox in Tools -> Settings...) removes all
restrictions: paths are no longer confined to the workspace, the shell command
ban list is skipped, and `allow_shell` is implied. Use it only on a disposable
VM. `session_info` reports `sandbox: DISABLED` when it is active.

## Safety
- All paths stay under workspace (resolve_path) unless `full_access=1`.
- Network tools are off unless `[net] allow=1`; optional `[net] allow_hosts` is a
  comma-separated host/suffix allow-list. Bodies are capped at `[net] max_bytes`
  (default 256 KB). `http_request` GET/HEAD do not prompt; methods that send
  data, `gist_upload` and applying `replace_across` do.
- `snapshot` copies are stored under `.snapshots\` and are skipped by search tools.
- Destructive tools require GUI/CLI confirmation.
- Tool argument logs redact api_key / password / token / secret patterns.
- `run_command` refuses common disk-wipe style command patterns.
- `rmdir_force` normalises the workspace root first, so a relative or
  trailing-slash `workspace=` in the INI cannot be deleted recursively.

## apply_patch example
```
*** SEARCH
int x = 1;
*** REPLACE
int x = 2;
*** END
```
