# scroll-options

Tiny Windows tray utility that picks how the mouse wheel scrolls. Single small
exe, no dependencies, no UI beyond a tray icon with a context menu.

| Mode | What it does |
| --- | --- |
| **Discrete** (default) | Native notch-by-notch scrolling, the Windows default. High-resolution input (trackpads, free-spinning wheels) is rounded to whole notches. |
| **Smooth** | macOS-like scrolling: each wheel notch glides to a stop with an ease-out, and quick flicks accelerate. Input that is already high-resolution is left as is. Ctrl+wheel zoom keeps whole steps. |
| **Passthrough** | Doesn't touch wheel input at all. Use it when you stream to this PC with Moonlight/Sunshine from a Mac, so the trackpad scrolls exactly as it does on the Mac. |

Discrete and smooth work through a low-level mouse hook, so they are only
active while the tray app is running. Discrete and smooth also switch the
*Smooth-scroll list boxes* visual effect off/on to match; passthrough leaves it
alone. The chosen mode is saved under `HKCU\Software\scroll-options` and
restored when the tray app starts.

Smooth mode sends wheel deltas smaller than one notch. Modern apps (browsers,
Office, VS Code, Explorer) scroll with them pixel by pixel; a few old apps
treat every wheel message as a full notch and will scroll too fast.

## Usage

Run `scroll-options.exe` with no arguments to put it in the tray. Click the
icon to choose a mode or **Exit**.

From cmd / PowerShell:

```
scroll-options discrete      # notch-by-notch scrolling
scroll-options smooth        # macOS-like smooth scrolling
scroll-options passthrough   # leave wheel input alone
scroll-options toggle        # switch between smooth and discrete
scroll-options status        # print the current mode
```

The running tray app picks up the change right away.

The exe is a GUI app so the tray never flashes a console. That means an
interactive shell doesn't wait for it, and its output can land after the
prompt. In scripts that need the exit code, use `start /wait scroll-options ...`
(cmd) or `Start-Process scroll-options -ArgumentList smooth -Wait` (PowerShell).

To start it with Windows, put a shortcut in `shell:startup`.

### Switching automatically with Sunshine

Sunshine can run commands when a client connects and disconnects. In the
Sunshine web UI under **Configuration → General → Command Preparations**, add:

- Do: `C:\path\to\scroll-options.exe passthrough`
- Undo: `C:\path\to\scroll-options.exe smooth` (or `discrete`)

## Build

Needs MinGW-w64:

```
make
```

## Releases

Every push to `main`/`master` builds `scroll-options.exe` on GitHub Actions and
publishes it as release `v2.0.<run number>`.
