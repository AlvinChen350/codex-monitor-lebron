# Codex Monitor

C++17 usage monitor for Windows 10 1809+ and Windows 11.

Checks each configured Codex account every 30 minutes. Captures three status cards and sends the latest to Discord as PNG and UTF-8 text. Reports are also saved in `reports`.

Requires Git, CMake 3.24+, Visual Studio 2022 Build Tools with Desktop development with C++, and an authenticated Codex CLI. CMake downloads a pinned libvterm revision during the first build.

Open PowerShell in this folder:

```powershell
.\build.ps1
codex
```

Finish Codex login and any first-run setup, then exit Codex.

In Discord, create a channel webhook under Server Settings → Integrations → Webhooks. Copy its URL.

```powershell
.\run.ps1 -Once -NoDiscord -NoAnchor
.\run.ps1 -Once
.\run.ps1
```

The second command asks for the webhook URL. The URL is kept in the current PowerShell environment, not written to a file. Ctrl+C stops the monitor. Windows must remain awake for scheduled checks.

Edit `config.ini`, or copy it to `config.local.ini` for private settings. Paths are relative to the configuration file. Windows environment variables such as `%USERPROFILE%` are supported in paths.

```ini
[account:Alt]
executable=codex
codex_home=%USERPROFILE%\.codex-alt
working_directory=.
```

Authenticate each account under its configured `CODEX_HOME` first. Point `executable` to the full `codex.exe` or `codex.cmd` path if it cannot be found.

`auto_anchor=true` sends one small model request when a full five-hour window appears dormant, then refreshes the status three times. This uses some allowance and relies on observed reset behavior, which is not guaranteed. Set `auto_anchor=false` or pass `-NoAnchor` for passive monitoring. `-NoDiscord` only disables uploads.

Each check uses a separate Codex process with read-only sandboxing and approval requests disabled. It does not resume coding goals. It leaves existing Codex sessions alone.

The status-card format and startup prompt must match the current parser. For a capture timeout:

```powershell
.\build\Release\codex-monitor.exe --once --no-discord --no-anchor --debug
```

Discord receives the full visible status card, which may include an account identifier or local directory. Use a private channel. If image rendering fails, the text attachment is still sent.

Based on [hschi1106/codex-monitor](https://github.com/hschi1106/codex-monitor). Terminal parsing uses [libvterm](https://github.com/neovim/libvterm), revision `934bc2fbf21800ac3458a499df8820ca5fb45fd3`.

Core parser tests were run on Linux. The live Windows, Codex, and Discord integration still needs a Windows smoke test. The included GitHub workflow builds the Windows executable and runs the parser tests.
