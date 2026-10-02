# QLIC WIC decoder

This read only Windows Imaging Component codec serves Explorer and WIC apps. It
supports these formats:

- 8-bit Gray, RGB, straight RGBA, and premultiplied PRGBA;
- 16-bit Gray, RGB, straight RGBA, and premultiplied PRGBA;
- packed 10-bit HDR10 when QSW2 declares full-range BT.2020/PQ RGB.

HDR10 output keeps the original 10-bit RGB values in WIC's
`R10G10B10A2HDR10` layout with opaque alpha. Other 9--15-bit and all 17--24-bit
images are rejected instead of quantized. The C, Rust, and Web APIs return the
exact samples.

WIC has no equivalent native HLG pixel format. A 10-bit HLG file is therefore
rejected by WIC instead of being mislabeled as HDR10; the QLIC GUI can show a
labeled SDR preview, and the C, Rust, and Web APIs retain its exact HLG samples
and CICP metadata.

## Install

Double-click `install-wic.cmd` in the packaged WIC folder. Setup checks the
bundle, requests administrator permission, installs it machine-wide, and
verifies registration. Running it again repairs or upgrades the installation.

For scripted setup:

```powershell
.\install-wic.ps1
```

The installer copies the decoder, GUI, CLI, and runtime files to a stable
content addressed folder before registration. Moving the downloaded package
does not break the install. Double-click `uninstall-wic.cmd` or run
`uninstall-wic.ps1` to remove it.

QLIC doesn't offer a per user WIC install. Windows' WIC component enumerator
doesn't treat per user COM keys as an installed codec either.

## Open files

The default `.qlic` command opens the original file in the QLIC GUI. Its viewer
decodes off the UI thread, preserves the filename, composites straight and
premultiplied alpha, applies embedded ICC profiles through Windows Color
Management when available, plays RGBA8 animation, and accepts 8--24-bit
integer stills. PQ/HLG content is labeled as an SDR preview. Opening it never
changes the stored samples or any of the metadata.

If the GUI is absent, registration uses Windows Photo Viewer where available.

**Open in Microsoft Photos (compatibility)** is optional. Microsoft Photos does
not declare `.qlic` in its signed manifest, so the command creates an exact-byte
private `.qlic.png` alias in `%TEMP%\QLIC-Photos`. It does not transcode or
rewrite metadata. This is also used as the fallback where Photo Viewer is unavailable.

## Color, alpha, and metadata

Embedded ICC profiles are returned through frame- and decoder-level WIC color
contexts. QLIC retains CICP, but general WIC integer formats do not carry it;
embed ICC for non-HDR10 color-managed WIC use.

Straight and premultiplied alpha use distinct WIC formats. The decoder does not
premultiply, unpremultiply, tone-map, or hide transparent RGB.

Frames implement `IWICMetadataBlockReader` and a WIC metadata query reader.
Valid EXIF/IFD, XMP, and IPTC use Windows handlers. JUMBF and nonstandard
payloads remain available through the opaque metadata handler. The SDK and CLI
sidecars are the byte-exact archival interface.

PNG `pHYs` metadata is reported by `GetResolution`. QLIC has no embedded
thumbnail; Explorer may scale the decoded frame. Capability probing validates
the file under WIC limits without consuming the caller's stream. The decoder
supports concurrent COM callers after synchronized initialization.
