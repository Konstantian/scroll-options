# scroll-options

Tiny Windows tray app that slows down scrolling on a PC you stream to with
Moonlight/Sunshine.

Native scrolling over Moonlight feels right but goes too far, even at one
line per notch in Windows. scroll-options multiplies every scroll event that
comes in over Sunshine by a speed you pick (10–100% in the tray menu, 5–200%
from the command line) and passes it on right away, with no smoothing. Mice
plugged into the PC itself and Ctrl+wheel zoom are left alone.

## Usage

Run `scroll-options.exe` and pick a speed from the tray icon. Or from the
command line:

```
scroll-options speed 60
scroll-options status
scroll-options version
```

**Autostart** in the tray menu adds the exe, from wherever it is,
to `HKCU\...\CurrentVersion\Run`, so keep it somewhere permanent first. To
update, exit the running copy and replace the exe. **About** in the same menu
shows the running version.

Sunshine input is recognised on its own: Sunshine either injects the Mac's
input (flagged as injected in Windows) or sends it through its virtual
"libvirtualhid Mouse" (USB ID 1209:0003). Other programs that inject scroll
events would also be scaled.

## Build

Needs MinGW-w64: `make` (or `make VERSION=1.2.3`). Every push to `main` is
released on GitHub as `v1.0.<run number>`, with the exe named
`scroll-options-<version>-x64.exe`.
