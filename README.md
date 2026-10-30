# scroll-options

Tiny Windows tray app that makes a Mac trackpad scroll naturally on a PC you
stream to with Moonlight/Sunshine.

- **Auto** (default): passthrough for scrolling that comes in over Sunshine,
  while mice plugged into the PC itself keep scrolling natively. No need to
  switch modes when you sit down at the PC.
- **Native**: leaves Windows alone.
- **Passthrough**: scales trackpad scrolling down 5× and smooths it out, so
  swipes move about as far as on the Mac. A real mouse wheel and Ctrl+wheel
  zoom are left untouched.
- **Scaled**: separate from the modes above. Scrolling from Sunshine keeps
  its native feel but is multiplied by **Scaled speed** (20–100% in the tray
  menu, 5–200% from the command line), with no smoothing. Use it when native
  scrolling is fine apart from being too fast, even at one line per notch in
  Windows. Local mice and Ctrl+wheel zoom are left alone.

Moonlight limits each scroll event it sends to one wheel notch, so very fast
flicks come out a little shorter than on the Mac.

## Usage

Run `scroll-options.exe` and pick a mode from the tray icon. Or from the
command line:

```
scroll-options auto
scroll-options native
scroll-options passthrough
scroll-options scaled
scroll-options speed 60
scroll-options toggle
scroll-options status
scroll-options version
```

**Start with Windows** in the tray menu adds the exe, from wherever it is,
to `HKCU\...\CurrentVersion\Run`, so keep it somewhere permanent first. To
update, exit the running copy and replace the exe. **About** in the same menu
shows the running version.

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
released on GitHub as `v1.0.<run number>`, with the exe named
`scroll-options-<version>-x64.exe`.
