# Ditto CLI

This fork exposes a local-only named-pipe control channel from the running desktop Ditto process.

## Commands

```powershell
DittoCli.exe set "你好"
"hello" | DittoCli.exe set -
DittoCli.exe get
DittoCli.exe ping
```

- `set` writes text to the real Windows clipboard in Ditto's interactive desktop session and asks Ditto to show a tray notification.
- `get` returns the current Windows clipboard text as raw UTF-8.
- `ping` verifies that the desktop Ditto process and its CLI pipe server are running.
- Successful `set` intentionally writes nothing to stdout.

The pipe is local-machine only (`PIPE_REJECT_REMOTE_CLIENTS`) and does not require Ditto's network-sharing password.
