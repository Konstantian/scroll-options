# scroll-options

Tiny Windows tray app that makes a Mac trackpad scroll naturally on a PC you
stream to with Moonlight/Sunshine.

- **Native** (default): leaves Windows alone.
- **Auto**: passthrough for scrolling that comes in over Sunshine, while mice
  plugged into the PC itself keep scrolling natively. No need to switch modes
  when you sit down at the PC.
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
scroll-options auto
scroll-options passthrough
scroll-options toggle
scroll-options status
scroll-options version
```

To start it with Windows, put a shortcut in `shell:startup`.

Auto tells the two apart on its own: Sunshine either injects the Mac's input
(flagged as injected in Windows) or sends it through its virtual "libvirtualhid
Mouse" (USB ID 1209:0003). Other programs that inject scroll events would also
count as Sunshine.

To switch modes with the stream instead, add these under Sunshine's **Configuration → General
→ Command Preparations**:

- Do: `C:\path\to\scroll-options.exe passthrough`
- Undo: `C:\path\to\scroll-options.exe native`

For the most direct feel, turn off smooth scrolling in your browser.

## Build

Needs MinGW-w64: `make` (or `make VERSION=1.2.3`). Every push to `main` is
released on GitHub as `v1.0.<run number>`.
