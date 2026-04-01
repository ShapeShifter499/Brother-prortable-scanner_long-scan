# brscan5-longpaper

Long paper scanning for Brother DS-740D and ADS-1200 scanners on Linux.

> **Warning:** Experimental / vibe-coded. There may be bugs.
> Test with low-value documents first.

## Background

On Windows, the Brother driver supports long paper scanning — receipts, shipping labels, and other documents longer than US Letter / A4. On Linux, the `brscan5` SANE driver never sends the required protocol commands, so feeding anything longer than ~14 inches causes a false paper-jam error and aborts the scan.

This project reverse-engineers the Windows long paper USB protocol and implements it for Linux via a native SANE backend.

## Status

| Hardware | Status |
|---|---|
| Brother DS-740D | ✅ Tested and working (Arch Linux) |
| Brother ADS-1200 | ⚠️ Implemented, not yet tested |

**Platform:** Arch Linux (and derivatives). Should work on any distro with `brscan5` and `sane` installed; only the AUR package is provided for now.

## How it works

A SANE backend (`brother5lp`) registers alongside the normal `brother5` backend. Scanning apps see the scanner as an additional device:

```
Brother DS-740D            ← original brscan5 (normal scans)
Brother DS-740D [Long Paper]  ← this project
```

Select the **\[Long Paper\]** device and the backend handles everything internally:

- Injects `PTYPE=LONGPAPER_WIDE`, `LONG=ON`, `LSMD=ON` into the USB scan commands
- Extends the scan area beyond the driver's built-in ADF cap
- Stops automatically when the paper exits the ADF — same behaviour as Windows
- Trims trailing blank rows from the output image
- Exposes up to 5000 mm br-y range to the frontend; set page height in your scanning app to control the maximum scan length

No `LD_PRELOAD`, no patching of system files, no configuration beyond selecting the device.

## Installation

### AUR (Arch Linux)

```bash
git clone https://github.com/ShapeShifter499/Brother-prortable-scanner_long-scan
cd Brother-prortable-scanner_long-scan/pkg
makepkg -si
```

The package depends on `brscan5` and `sane`. After install, `brother5lp` is added to `/etc/sane.d/dll.conf` automatically.

### Manual (other distros)

```bash
git clone https://github.com/ShapeShifter499/Brother-prortable-scanner_long-scan
cd Brother-prortable-scanner_long-scan
make
sudo make install
echo 'brother5lp' | sudo tee -a /etc/sane.d/dll.conf
```

Requires: `gcc`, `libsane-dev` (or `sane`), `libusb-1.0-dev`.

## Usage

### GUI (Simple Scan, XSane, gscan2pdf, GIMP)

1. Open your scanning app
2. Select **Brother DS-740D \[Long Paper\]** (or equivalent) as the device
3. Set page height to the maximum you want to scan (e.g. 1000 mm for a long receipt)
4. Feed the document — scanning stops automatically when the paper exits

### CLI

```bash
scan-long --output receipt.png
```

Options:

```
--output FILE       Output file (default: scan_YYYYMMDD_HHMMSS.png)
--mode WIDE|NARROW  Paper width mode (default: WIDE)
--resolution DPI    Scan resolution (default: 300)
--length MM         Maximum scan length in mm (default: 1829 / 72 inches)
--debug             Print verbose USB command log to stderr
```

## Building from source

```bash
make          # builds libbr5longpaper.so and libsane-brother5lp.so.1
make install  # installs to /usr/local/lib and /usr/local/lib/sane
```

## Troubleshooting

Run with `BROTHER_SCAN_DEBUG=1` to see USB command logs on stderr. Key lines to look for:

```
[brother5lp] promoted to RTLD_GLOBAL          ← hooks active
[brother5lp] loaded brscan5 via dlopen        ← brscan5 wrapped
[brother5lp] USB patch SSP: LONGPAPER_WIDE    ← protocol patched
[brother5lp] DeviceImageJpeg::SetHeight ...   ← height cap bypassed
[brother5lp] BitmapImage::AppendWhiteLines suppressed  ← no blank rows
```

If `promoted to RTLD_GLOBAL` is missing, the USB and C++ hooks will not fire.

## Contributing

Testing reports — especially for the ADS-1200 — are very welcome. Open an issue with the debug log output and a description of what did or didn't work.

## License

GPLv2. See [LICENSE](LICENSE).
