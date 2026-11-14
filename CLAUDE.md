# CLAUDE.md — HB Browser

Guidance for Claude (and other coding agents) working in this repository.
Items marked `TODO` are not verified yet. Fill them in or correct them.

## Project

- **Name:** HB Browser, by Homebax Studios (Frýdek-Místek, CZ)
- **License:** GPL-3.0. Free and open source.
- **Version:** TODO (check `CMakeLists.txt` / `info.json`; last known target 2.1.2)
- **Platforms:** Windows desktop (this repo). A working Android version exists separately (TODO: repo/path).
- **Goals:** small (max ~1–2 GB), fast, friendly to low-end PCs, portable (no installer), works offline for UI assets.

## Tech stack

- Language: C++ with Win32 (`windows.h`), built with CMake. IDE: Visual Studio 2022.
- Rendering: Microsoft WebView2 (TODO: confirm this is still the case; earlier plans used a custom engine, "HB Core").
- Third-party libraries (see `thirdparty/README.md`): WebView2, nlohmann/json, SQLite, spdlog, WIL.
- No Electron, no installer, no runtime downloads of icons or fonts.

## Repository layout

TODO: replace with the real tree. Expected shape:

```
src/browser/      C++ sources (window, tabs, omnibox, settings, bookmarks, memory)
thirdparty/       Vendored libraries + README.md
assets/           Offline icons, app icon
CMakeLists.txt
README.md         What is where
TUTORIAL.md       How to build the .exe and configure things
```

## Build and run

TODO: confirm exact commands. Typical flow:

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

- Target: Windows 7+ if feasible. Support x64, x86 and ARM64 where possible.
- Output must stay portable: one folder, no installer, no registry dependency.

## Features that must keep working

- Tabs formatted as `{site icon} {site title} ×`, with working close button
- Omnibox with autocomplete suggestions while typing
- Configurable search engine (default Google)
- Pinned sites under the input, toolbar, bookmarks, settings
- Colour theming (default: follow system)
- Right-click context menu near the cursor (back, forward, reload, copy all, view source)
- Keyboard shortcuts (Ctrl+Z/Y/R/A/C/V etc.)
- Auto-update check: fetch `https://homebax.vercel.app/browser/info.json`, compare `version`, open `update-url`
- Memory feature (stored locally, SQLite)
- Chrome-compatible extensions (planned, TODO: status)

## Design rules

- UI/UX modeled on ChatGPT Atlas: modern, calm, rounded, moderate animations. No AI features in the UI.
- App icon: soft and round, no sharp edges.
- All icons bundled offline. Never load UI assets from the network.
- Default to the system theme.

## Code conventions

- Modern C++ (TODO: standard, e.g. C++20). Prefer RAII (WIL) over manual handle management.
- Logging via spdlog. No `printf`-style debugging left in commits.
- JSON via nlohmann/json, persistence via SQLite. Keep storage paths relative to the app folder (portable).
- Keep dependencies minimal. Ask before adding a new library, because size and speed are core goals.
- Keep UI text localizable (Czech and English at least).

## Working rules for agents

- Read `README.md` and `TUTORIAL.md` first. Keep them in sync when files move or build steps change.
- Make small, focused changes. Do not reformat unrelated code.
- Do not add network calls other than the update check and what the user's browsing requires.
- After changes, make sure the project still configures and builds. State clearly if you could not test.
- Do not commit binaries, build output (`build/`), or secrets.

## Known issues / TODO

TODO: list open bugs and planned work (e.g. features to port from the Android version).
