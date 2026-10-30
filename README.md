# scroll-options

Tiny Windows tray app that makes a Mac trackpad scroll naturally on a PC you
stream to with Moonlight/Sunshine.

- **Native** (default): leaves Windows alone.
- **Passthrough**: scales trackpad scrolling down 5× and smooths it out, so
  swipes move about as far as on the Mac. A real mouse wheel and Ctrl+wheel
  zoom are left untouched.

Moonlight limits each scroll event it sends to one wheel notch, so very fast
flicks come out a little shorter than on the Mac.

## Usage

Run `scroll-options.exe` and pick a mode from the tray icon. Or from the
command line:

```
scroll-options native
scroll-options passthrough
scroll-options toggle
scroll-options status
scroll-options version
```

To start it with Windows, put a shortcut in `shell:startup`.

To switch automatically, add these under Sunshine's **Configuration → General
→ Command Preparations**:

- Do: `C:\path\to\scroll-options.exe passthrough`
- Undo: `C:\path\to\scroll-options.exe native`

For the most direct feel, turn off smooth scrolling in your browser.

## Build

Needs MinGW-w64: `make` (or `make VERSION=1.2.3`). Every push to `main` is
released on GitHub as `v1.0.<run number>`.
