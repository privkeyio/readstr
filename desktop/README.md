# Readstr for Omarchy

A native desktop Readstr for [Omarchy](https://omarchy.org): one window,
keyboard first, following the Omarchy theme and language live, and installed
as an Arch package so it shows up in the app launcher. It is built with C++
and Qt Quick, following the Omarchy app conventions. See
[`docs/OMARCHY_APP_PLAN.md`](../docs/OMARCHY_APP_PLAN.md) for the plan.

This is phase 0, the scaffold. The window is themed but empty, and reading
feeds arrives in phase 1.

## Build, test, install

Everything needed comes with Omarchy (`qmake6`, `make`, a C++ compiler, and
Qt's base, declarative, SVG, and Wayland modules).

```sh
bin/build      # qmake6 + make into build/readstr
bin/test       # builds and runs the Qt Test suite offscreen
bin/install    # makepkg -fsi, so Readstr appears in the launcher (Super + Space)
```

Run `build/readstr` (or `build/readstr open`) to open the window.

## Command line

Anything other than `open` runs headless, prints errors on stderr, and exits
0 or 1.

```sh
readstr --version          # readstr 0.1.0
readstr version --json     # {"name":"readstr","qt":"6.12.0","version":"0.1.0"}
```

`READSTR_HOME` moves the data, config, and cache directories under one
directory, which is how tests avoid touching your real files.

## Shortcuts

| Key | Action |
|---|---|
| `?` | Show or hide the shortcuts |
| `Esc` | Close the shortcuts |
| `Q`, `Ctrl+Q` | Quit |

## License

MIT, see [`LICENSE`](LICENSE). The theme and language following code
comes from Omacut and Monologue (MIT, David Heinemeier Hansson).
