# Local Workspace

This directory is ignored except for this file. Keep private App checkouts,
temporary factory overlays and App build output here, or point to directories
outside this repository.

Put validation reports, benchmarks, measurements, logs, screenshots, experimental
data and task notes in `local/artifacts/<topic>/<YYYYMMDD-HHMMSS>/`. These outputs,
including Markdown reports, stay local unless the user explicitly asks to version
a particular report. `docs/` holds maintained documentation and release notes.
Historical reports and their supporting files are in `local/artifacts/docs/`,
with their original directory structure preserved.

Create `local/apps.toml` to map an App ID to a source root. Every source root
must contain a directory with the same name as the App ID.

```toml
[apps.pai-touch-diagnostics]
source_root = "/home/user/work/pxa-apps"

[apps.private-lab]
source_root = "/home/user/work/private-pxa-apps"
```

An untracked factory overlay can append private Apps:

```toml
[factory]
apps = ["private-lab"]
```
