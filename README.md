# smooth-scroll

Tiny Windows tray utility that switches between **smooth** and **discrete**
(native line-by-line) scrolling. Single ~20 KB exe, no dependencies, no UI
beyond a tray icon with a context menu.

It flips the system setting *Smooth-scroll list boxes*
(System Properties → Performance Options → Visual Effects) via
`SPI_SETLISTBOXSMOOTHSCROLLING`. The change is saved to your user profile, so
it persists after the app exits or Windows restarts.

## Usage

Run `smooth-scroll.exe` with no arguments to put it in the tray. Click the icon
to choose **Smooth scrolling** / **Discrete scrolling** or **Exit**.

From cmd / PowerShell:

```
smooth-scroll discrete   # enable discrete scrolling
smooth-scroll smooth     # enable smooth scrolling
smooth-scroll toggle     # switch between the two
smooth-scroll status     # print "smooth" or "discrete"
```

If the tray app is running, its tooltip and menu update right away.

The exe is a GUI app so the tray never flashes a console. That means an
interactive shell doesn't wait for it, and its output can land after the
prompt. In scripts that need the exit code, use `start /wait smooth-scroll ...`
(cmd) or `Start-Process smooth-scroll -ArgumentList discrete -Wait` (PowerShell).

To start it with Windows, put a shortcut in `shell:startup`.

## Build

Needs MinGW-w64:

```
make
```

## Releases

Every push to `main`/`master` builds `smooth-scroll.exe` on GitHub Actions and
publishes it as release `v1.0.<run number>`.
