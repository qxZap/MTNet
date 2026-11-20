# MTNet

A Motor Town plugin that sits in front of the game's network layer and gives you
an in-browser control panel for its console variables.

MTNet points the game's host and web server URLs at a local proxy, so it can read
the config the server hands out and merge your own console-variable overrides on
top of it. A tablet-style panel — press **CTRL+F9** to open it in your browser —
lets you:

- see and edit the live console variables the server is running
- browse the full engine catalogue (2,500+ names, Motor Town's own `mh.*` among
  them) and add any of them
- save and load templates (config presets — a few examples ship in
  `mt_net_templates\`)
- watch what the server pushes at the game (feedback / leaderboard / news)

On a dedicated server the panel is reachable too, protected by a password written
to `mt_net.ini` on first run.

Two switches in `plugins\mt_net.ini`, `[MTNet]`:

- `Dump=1` records every exchange between the game and its servers in full, request headers and body, then the
  whole response, to `plugins\logs\MTNet	raffic\<session start>\` (one file each, plus `index.tsv`). Off by
  default: the files carry the session's auth headers, so do not share them as they are.
- `ShowHidden=1` (default) keeps servers the backend marks hidden (`"hide":2` in `/hosts`; more than 100 slots) in the
  game's server list.

A dedicated server and a game on the same PC need different `Port` / `WebPort` / `UiPort` values: two copies on the
same ports share them, and the game's requests reach the wrong one.

## Install

Needs the [Plugin Loader](https://github.com/qxZap/mt-plugin-loader)
(`version.dll`) in `MotorTown\Binaries\Win64\`. Then copy from a build (or a
release) into the game:

| file | goes in |
|------|---------|
| `mt_plugin_MTNet.dll` | `MotorTown\Binaries\Win64\plugins\` |
| `mt_net_panel.html`   | `MotorTown\Binaries\Win64\` |
| `mt_net_templates\`   | `MotorTown\Binaries\Win64\` (optional presets) |

Launch the game and press **CTRL+F9** to open the panel.

## Build

`build.bat` (MSVC 2022, x64) writes `out\mt_plugin_MTNet.dll`. It compiles the one
plugin against the loader headers in `include\` and `src\ini_merge.h`; nothing
else is needed.
