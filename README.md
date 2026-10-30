# scroll-options

Tiny Windows tray utility for scrolling on a PC you stream to from a Mac with
Moonlight/Sunshine. Single small exe, no dependencies, no UI beyond a tray
icon with a context menu.

| Mode | What it does |
| --- | --- |
| **Native** (default) | Leaves Windows alone: no hook, no settings changed. |
| **Passthrough** | Makes the Mac's trackpad (or Magic Mouse) scroll on this PC about the way it does on the Mac. Use it while streaming with Moonlight. |

## Why passthrough is needed

Moonlight on macOS sends one Windows wheel notch per ~10 pt of trackpad
scrolling, and Windows apps scroll three lines (~100 px in a browser) per
notch. Without help, a slow swipe travels about ten times as far as it does
on the Mac. Passthrough:

- scales trackpad deltas down 5×, so swipes move roughly as far as on the Mac;
- replays them evenly once per display frame, smoothing out network jitter;
- leaves a real mouse wheel (whole notches), Ctrl+wheel zoom and windows it
  can't inject into (elevated apps such as Task Manager) untouched.

Moonlight also clamps each event to one notch, so very fast flicks come out
a little shorter than on the Mac. If the speed doesn't feel right, pick
another value under **Passthrough speed** in the tray menu, or run
`scroll-options speed 150`.

Passthrough works through a low-level mouse hook, so it's only active while
the tray app is running. Settings are saved under
`HKCU\Software\scroll-options` and restored when the tray app starts.

Things on the Windows side that still affect the feel:

- Browsers animate every wheel event, so scrolling can trail behind your
  fingers. For the most direct feel, turn smooth scrolling off:
  `chrome://flags/#smooth-scrolling`, `edge://flags/#smooth-scrolling`, or in
  Firefox, Settings → General → Use smooth scrolling.
- Sunshine's **High resolution scrolling** option has to stay on (the
  default). If Sunshine drives the mouse through its licensed Virtual HID
  Driver, it rounds scrolling to whole notches before Windows sees it, and
  passthrough can't restore that.

## Usage

Run `scroll-options.exe` with no arguments to put it in the tray. Click the
icon to choose a mode, the passthrough speed, or **Exit**.

From cmd / PowerShell:

```
scroll-options native        # leave scrolling alone
scroll-options passthrough   # Mac trackpad over Moonlight/Sunshine
scroll-options toggle        # switch between native and passthrough
scroll-options speed 150     # passthrough speed in percent (default 100)
scroll-options status        # print the current mode
```

The running tray app picks up the change right away.

The exe is a GUI app so the tray never flashes a console. That means an
interactive shell doesn't wait for it, and its output can land after the
prompt. In scripts that need the exit code, use `start /wait scroll-options ...`
(cmd) or `Start-Process scroll-options -ArgumentList passthrough -Wait`
(PowerShell).

To start it with Windows, put a shortcut in `shell:startup`.

### Switching automatically with Sunshine

Sunshine can run commands when a client connects and disconnects. In the
Sunshine web UI under **Configuration → General → Command Preparations**, add:

- Do: `C:\path\to\scroll-options.exe passthrough`
- Undo: `C:\path\to\scroll-options.exe native`

## Build

Needs MinGW-w64:

```
make
```

## Releases

Every push to `main`/`master` builds `scroll-options.exe` on GitHub Actions and
publishes it as release `v1.0.<run number>`.
