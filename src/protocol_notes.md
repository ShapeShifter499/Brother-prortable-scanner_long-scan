# Brother ADS/DS Scanner Protocol Notes

Reverse-engineered from `brscan5-1.5.1-0` package libraries.

## USB Identification

| Field      | Value                          |
|------------|-------------------------------|
| Vendor ID  | `0x04f9` (Brother Industries) |
| Interface  | Class `0xFF`, SubClass `0xFF`, Protocol `0xFF` (vendor-specific) |
| Transfer   | USB bulk (`libusb_bulk_transfer`) |

### Known Product IDs (from `brscan5ext_*.ini`)

| Model      | PID    | Capability type |
|------------|--------|-----------------|
| ADS-1200   | 0x0459 | 315             |
| ADS-1250W  | 0x045a | 315             |
| ADS-1700W  | 0x045b | 315             |
| ADS-2400N  | 0x03b7 | 315             |
| ADS-2800W  | 0x03b9 | 315             |
| ADS-3000N  | 0x03b8 | 319             |
| ADS-3600W  | 0x03ba | 319             |
| DS-740D    | ???    | ??? (in encrypted ini) |

The DS-740D is shipped in the same `brscan5-1.5.1-0` package as the ADS-1200
(same download IDs `dlf104033_000` / `dlf104034_000`), but its entry is in one
of the XOR-obfuscated `brscan5ext_*.ini` files (files 3–26 are obfuscated).

The XOR key for those files has not been fully recovered. The user must run
`lsusb | grep 04f9` with the DS-740D connected to determine its PID.

## brscan5 File Layout (after `dpkg -x brscan5-*.deb /opt/...`)

```
/opt/brother/scanner/brscan5/
├── brsaneconfig5               # CLI scanner registration tool
├── brscan5.ini                 # Main config
├── models/brscan5ext_*.ini     # Model capability tables
│   0,1,2: plaintext (ADS models)
│   3–26:  XOR-obfuscated
├── libsane-brother5.so.1.0.7   # SANE backend (loads core API)
├── libLxBsScanCoreApi.so.3.2.1 # Core scanning API
├── libLxBsUsbDevAccs.so.1.0.0  # USB device access layer
├── libLxBsNetDevAccs.so.1.0.0  # Network device access layer
└── libLxBsDeviceAccs.so.1.0.0  # Generic device access
```

## Scan Command Protocol (QDI text format)

Commands are sent as plain-text key=value pairs over USB bulk transfers.
Multiple commands types exist (CKD, ABT, XSC, SSP).

### Command Keys (extracted from `libLxBsScanCoreApi.so.3.2.1`)

**Group 1 — scan setup (CKD command)**
| Key    | Known values                                          | Notes                     |
|--------|-------------------------------------------------------|---------------------------|
| OS=    | `LNX`, `WIN`, `MAC`, `IOS`, `ADR`, `WPH`             | Operating system          |
| PSRC=  | (paper source values)                                 | Paper source / ADF type   |
| CLR=   | `C24BIT`, (others)                                    | Color mode                |
| AREA=  | `NORMAL`, `FULL`, `OVER`, `ATDSKW`                    | Area detection mode       |
| DPLX=  | `OFF`, (duplex values)                                 | Duplex mode               |
| COMP=  | `JSF`, `422`, `411`, `420`, `400`, `444`              | Compression               |
| IPRC=  | `COPY`, `TXTGR`                                       | Image processing          |
| PTYPE= | `THIN`, `THOCK`, `PCARD`, **`LONGPAPER_WIDE`**, **`LONGPAPER_NARROW`**, `DEVICEMAX`, `CUSTOMSIZE` | Paper type |
| LONG=  | **`ON`** (assumed; function takes bool)               | Long paper enable flag    |
| CARR=  |                                                       | Carrier sheet mode        |
| DSKW=  |                                                       | Deskew                    |
| LSMD=  |                                                       | Long scan mode detect     |

**Group 2 — resolution and processing (ABT/XSC command)**
| Key    | Notes                  |
|--------|------------------------|
| RESO=  | Resolution (DPI)       |
| GMMA=  | Gamma                  |
| RMBP=  | Remove blank pages     |
| DTDF=  | Double-feed detection  |
| RMGC=  | Remove gray channel    |
| PAGE=  | Page count             |
| BRIT=  | Brightness             |
| MRGN=  | Margin                 |

### Long Paper Values (from `libLxBsScanCoreApi.so` string table)

```
PTYPE=LONGPAPER_WIDE    # Wide long paper (A4/letter width ≈ 210–216mm)
PTYPE=LONGPAPER_NARROW  # Narrow long paper (receipt/ticket width)
LONG=ON                  # Enable long paper scan flag
```

The `LONGPAPER_WIDE` / `LONGPAPER_NARROW` identifiers come directly from the
internal `PAPERTYPEENUM` string table in the core library. The full enum is:

```
..., MEXICANLEGAL, INDIALEGAL, LONGPAPER_NARROW, LONGPAPER_WIDE,
DEVICEMAX, CUSTOMSIZE, PAPERTYPENUM
```

### Long Paper Functions in `libLxBsScanCoreApi.so`

```cpp
CAdjustScanSetting::ConvertUserSetting_LongPaper(
    const BS_SCANNER_PARAMETERS&, const BS_SCANNER_CAPABILITY&)

CCmdString_v2::MakeLongPaperModeString(bool)   // generates LONG= token
ScanCapability::GetLongPaperSupport(...)
ScanCapability::GetLongPaperMaxLength(...)
CReadPreparation_base::ChangeLongPaperIfNeededERNS(...)
CDeviceCapsForPreparation::GetLongPaperLengthMax()
CScanArea::GetLongPaperSizePixel(...)
```

## SANE Backend Constraints

`libsane-brother5.so` reads max ADF height from scanner capabilities in
0.01mm units and exposes it via the `br-y` SANE option range.
For ADS-1200 the reported max is approximately 291–355mm (non-long-paper mode).

The `libsane-brother5.so` exposes these scan-related SANE options:
- `mode` — color mode (Color, Grayscale, etc.)
- `resolution` — DPI
- `source` — paper source: `"Automatic Document Feeder(center aligned)"`, etc.
- `brightness`, `contrast`
- `tl-x`, `tl-y`, `br-x`, `br-y` — scan area
- `AutoDocumentSize` / `Auto Document Size`
- `Paper size detection`

There is **no** native SANE option for long paper mode in the current backend.

## What the Hook Does

`libbr5longpaper.so` intercepts two points:

1. **`sane_get_option_descriptor()`** — when `br-y` is queried, returns an
   extended range with `max = 5000mm` instead of the backend's default.

2. **`libusb_bulk_transfer()`** — when an outgoing bulk transfer contains scan
   parameters (`RESO=`, `CLR=`, `PTYPE=`), replaces/inserts:
   ```
   PTYPE=LONGPAPER_WIDE   (or LONGPAPER_NARROW)
   LONG=ON
   ```

## DS-740D PID Discovery

Run with the DS-740D connected:
```bash
lsusb | grep "04f9"
```
Example output:
```
Bus 001 Device 005: ID 04f9:04ab Brother Industries, Ltd DS-740D
```
PID here is `0x04ab`. Register it:
```bash
sudo brsaneconfig5 -a name=DS-740D model=DS-740D usb=0x04f9:0x04AB
```

## Open Questions (need hardware testing)

1. Does `LONG=` really take `ON`/`OFF`?  
   (`MakeLongPaperModeString(bool)` implies yes, but the exact string isn't
   stored as a literal in the binary — it's built at runtime.)

2. Does setting `br-y` above the reported max (before our hook extends it)
   already trigger `ChangeLongPaperIfNeeded` internally in brscan5?
   If yes, only the range extension in step 1 of the hook is needed.

3. What is the DS-740D's USB PID?

4. Does `AREA=FULL` need to accompany long paper, or is `AREA=NORMAL` + 
   `PTYPE=LONGPAPER_WIDE` sufficient?

5. What are the capability-type differences between `315` (ADS-1200) and
   `319` (ADS-3600W)?  Type `319` has extra ini fields `130,2`.

## Testing Procedure

```bash
# 1. Build the hook
make

# 2. Confirm scanner is detected (without hook)
scanimage -L

# 3. Check the br-y range reported without hook
scanimage --device 'brother5:...' --help 2>&1 | grep 'br-y'

# 4. Check br-y range with hook applied (should show 5000mm max)
BROTHER_LONG_MODE=WIDE LD_PRELOAD=./libbr5longpaper.so \
    scanimage --device 'brother5:...' --help 2>&1 | grep 'br-y'

# 5. Attempt a long scan (feed a long document through the ADF)
./scan_long.sh --length 1000 --output long_test.tiff

# 6. Verify output dimensions
identify long_test.tiff | grep 'x[0-9]'
```
