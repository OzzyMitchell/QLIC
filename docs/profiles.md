# QLIC profiles

QLIC has one container and several deliberately separate profiles.

Profile names and required features are fixed by the checked conformance manifest.
This is a stable reference source compatibility contract.

## Core Still 1

Core Still 1 is the portable lossless 8-bit still image profile. It is the
default profile for SDKs, browsers, asset pipelines, and the safe Rust decoder.

A Core Still 1 decoder must implement:

- the 28-byte checked QLIC container in [core-still.md](core-still.md);
- outer codecs Store and LZMS;
- outer modes 1 through 5, 7, 9, 13, and 14;
- the legal mode/transform combinations listed in `profiles.json`;
- QST1 native modes 0, 1, 37, 39, 40, 45, 52, 53, and 54;
- native transforms 0 through 40;
- QST1 adaptation values 4, 5, and 6;
- exact straight RGBA8 output and all required checksums;
- checked limits for input bytes, decoded bytes, pixels, nested chunks, and
  entropy payloads.

Core Still 1 never depends on PNG, WIC, libjxl, libwebp, a Windows compression
API, or floating point arithmetic. LZMS decoding is part of the profile and is
implemented in portable C and safe Rust in this repository.

The outer mode-14 band count is at most 65,536 in the wire format. A Core
Still 1 implementation must accept at least 256 bands when its configured
resource limits permit the decoded image. The default QLIC resource policy is
256 bands.

## Animation 1

Animation 1 adds outer mode 17 and QAN1/QAN2 frame programs. Nested images are
Core Still 1 files and cannot themselves be animations. A decoder that does
not implement Animation 1 must reject mode 17 as unsupported; it must not
return only the first frame.

## Wide Integer 1

Wide Integer 1 adds mode 19 and QSW1. It carries exact unsigned samples from 9
through 24 bits per channel. It does not assign color primaries, a transfer
function, signal range, or alpha association. The wire format is defined in
[format.md](format.md#native-wide-integer-samples).

## Self-Describing Integer / HDR 1

HDR 1 adds mode 20 and QSW2 version 1. It carries exact unsigned 8--24-bit
integer samples plus explicit alpha and ICC and/or CICP color metadata and
ordered opaque ancillary metadata blocks. At
8 bits it is the metadata-bearing still-image profile; above 8 bits it is the
self-describing counterpart to Wide Integer 1. The wire format is defined in
[hdr-format.md](hdr-format.md).

HDR 1 preserves metadata and samples. It does not tone-map, convert color,
premultiply alpha, interpret photographic records, or claim that unspecified
color is HDR. Current decoders retain ancillary blocks; older HDR-1 decoders
may skip them, as version 1 has always allowed.

## Legacy 0.x

Legacy 0.x covers historical outer modes 10 through 12, 15, 16, and 18 and
native modes outside the Core Still 1 list. The canonical C decoder retains
these readers for old files and fuzz coverage. New 1.0 encoders must not emit
Legacy 0.x syntax unless the caller explicitly asks for it.

Legacy support is not a license to reinterpret a value. Unknown modes,
transforms, flags, codecs, or critical chunks are errors.

## Capability reporting

Libraries and applications should report profile support as independent bits:

```text
core-still-1
animation-1
wide-integer-1
hdr-1
legacy-0x
```

An encoder and decoder can support different sets. For example, a browser may
decode Core Still 1 and HDR 1 while encoding nothing.

## Conformance

Conformance requires all of the following:

1. Decode every positive vector for the claimed profile to the recorded pixel
   or sample CRC.
2. Reject every negative vector with an error rather than partial output.
3. Enforce caller limits before an allocation whose size the rejected field
   controls.
4. Produce no output on checksum failure.
5. Match the arithmetic rules in the normative documents on every supported
   platform.

`profiles.json` is the machine-readable feature list.
[`tests/fixtures/manifest.json`](../tests/fixtures/manifest.json) records exact
file hashes and decoded checksums. `tests/make-manifest.ps1` regenerates it,
and the `qlic-conformance-manifest` test rejects changed or unlisted fixtures.
The C and safe Rust decoders are tested independently against those files.
