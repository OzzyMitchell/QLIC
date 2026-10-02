# C SDK

## Pick an API

| Data | Encode | Decode |
| --- | --- | --- |
| Gray8, GrayA8, RGB8, or RGBA8 still image | `qlic_encode_pixels` or `qlic_encode_rgba` | `qlic_decode_rgba` or `qlic_decode_pixels` |
| RGBA8 animation | `qlic_encode_animation` | `qlic_decode_animation` |
| Exact 9--24-bit integer samples | `qlic_encode_wide` | `qlic_decode_wide` |
| Integer HDR samples and metadata | `qlic_encode_hdr` | `qlic_decode_hdr` |
| Header only | — | `qlic_get_info_v2` |
| Full integrity check without retained output | — | `qlic_validate` |

Query the linked library to see what it supports so you don't need to work it out from the version string:

```c
qlic_capabilities capabilities = {0};
capabilities.struct_size = sizeof(capabilities);
if (qlic_get_capabilities(&capabilities) == QLIC_OK &&
    (capabilities.decode_profiles & QLIC_PROFILE_HDR)) {
  /* qlic_decode_hdr is available in this library. */
}
```

`qlic_validate` fully decodes the selected still, animation, wide, or HDR
grammar, verifies outer and inner checksums, and immediately releases decoded
storage. Use it for ingestion checks where `qlic_get_info_v2` is evidently
too shallow and the caller doesn't need the pixels.

Profiles are independent bits for Core Still, animation, wide integer, HDR,
and retained legacy syntax. Feature bits report threads, v2 resource limits,
portable LZMS, ICC, CICP, and photographic metadata-block support. The profile definitions are in
[profiles.md](profiles.md).

Complete buildable examples are in `examples/c/encode.c` and
`examples/c/decode.c`.

Include the public header.

```c
#include <qlic/qlic.h>
```

With CMake, link the static library or DLL import target.

```cmake
find_package(qlic 1.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE qlic::qlic_static)
```

Use `qlic::qlic` for the DLL. The static package uses the static MSVC runtime.

Projects that do not use CMake can discover the installed shared library with
`pkg-config`:

```sh
cc app.c -o app $(pkg-config --cflags --libs qlic)
```

The installed `qlic.pc` is relocatable and reports the same version as
`qlic_version()`.

## Encode

```c
qlic_encode_options options;
qlic_encode_options_default(&options);
options.threads = 1;

uint8_t *encoded = NULL;
size_t encoded_size = 0;
int status = qlic_encode_rgba(
    rgba, rgba_size, width, height, stride, &options,
    &encoded, &encoded_size);
if (status == QLIC_OK) {
  qlic_free(encoded);
}
```

There is only one automatic encode policy, by design, of course. `qlic_encode_options`
controls only the worker limit; `flags` and `reserved` must be zero. Internal
benchmark builds might compare alternative search or decode-cost rules, but the
public C API uses one automatic policy.

The options struct remains 16 bytes. Initialize it with
`qlic_encode_options_default` so future additive fields stay safe.

The byte count has to cover the final row, and the stride has to cover one RGBA row. A thread count of zero means one thread. Values above the available hardware count are clamped. Files from this tree use the same decoder on every supported platform

## Exact 9 through 24-bit samples

Use the wide API when reducing an asset to RGBA8 would lose information.
Channels may be 1, 3, or 4 and every channel uses the same declared precision.
For 9..16 bits, pass native-endian `uint16_t` samples. For 17..24 bits, pass
native-endian `uint32_t` samples. This API writes native QLIC/QSW1 payloads;
it neither embeds nor calls PNG. PNG16 is only an optional command-line file
adapter for applications that already use that asset format.

```c
uint8_t *encoded = NULL;
size_t encoded_size = 0;
int status = qlic_encode_wide(
    samples, samples_size, width, height, stride,
    channels, bits_per_sample, NULL, &encoded, &encoded_size);
if (status == QLIC_OK) {
  qlic_free(encoded);
}
```

Decode without a precision conversion and free the matching wide object.

```c
qlic_wide_image image = {0};
int status = qlic_decode_wide(data, size, NULL, &image);
if (status == QLIC_OK) {
  /* image.pixels is uint16_t[] or uint32_t[] according to bits_per_sample. */
  qlic_wide_image_free(&image);
}
```

`qlic_decode_rgba` returns `QLIC_UNSUPPORTED_FORMAT` for a native-wide file;
it never truncates or rescales it. Initialize `qlic_info_ex.struct_size` and
call `qlic_get_info_ex` to inspect channels and precision without decoding.
The existing `max_payload_bytes` decode limit also caps decoded wide sample
storage, preserving the original limits-struct ABI.

## Self-describing integer HDR

Mode 19 preserves code values but intentionally has no color interpretation.
Use the additive HDR API when a file must also retain its transfer function,
primaries, range, mastering information, and alpha association.

```c
qlic_hdr_image hdr = {0};
hdr.struct_size = sizeof(hdr);
hdr.width = width;
hdr.height = height;
hdr.channels = 4;
hdr.bits_per_sample = 12;
hdr.sample_type = QLIC_SAMPLE_UINT;
hdr.alpha_mode = QLIC_ALPHA_STRAIGHT;
hdr.color_authority = QLIC_COLOR_CICP;
hdr.pixels = samples;
hdr.pixels_size = samples_size;
hdr.stride = stride;
hdr.has_cicp = 1;
hdr.cicp.color_primaries = QLIC_CICP_PRIMARIES_BT2020;
hdr.cicp.transfer_characteristics = QLIC_CICP_TRANSFER_PQ;
hdr.cicp.matrix_coefficients = QLIC_CICP_MATRIX_RGB;
hdr.cicp.full_range = 1;

qlic_metadata_block metadata[2] = {0};
memcpy(metadata[0].tag, "EXIF", 4);
metadata[0].data = exif;
metadata[0].size = exif_size;
memcpy(metadata[1].tag, "XMP_", 4);
metadata[1].data = xmp;
metadata[1].size = xmp_size;
hdr.metadata = metadata;
hdr.metadata_count = 2;

uint8_t *encoded = NULL;
size_t encoded_size = 0;
int status = qlic_encode_hdr(
    &hdr, NULL, &encoded, &encoded_size);
qlic_free(encoded);
```

Use `QLIC_CICP_TRANSFER_HLG` for Rec. 2100 HLG. The PQ and HLG constants map
to ISO/IEC 23091-2 transfer-characteristic values 16 and 18. Encoding and
decoding preserve samples and CICP values exactly; QLIC does not render or
tone-map them.

`QLIC_COLOR_ICC`, `QLIC_COLOR_CICP`, and the two preferred forms enforce that
the corresponding ICC/CICP fields are actually present. When both descriptions
exist, the preferred value is authoritative and both are preserved. Optional
mastering-display and MaxCLL/MaxFALL structs are copied exactly.
Ordered ancillary blocks preserve their four-byte tags and payloads exactly;
the conventional photographic tags are `EXIF`, `XMP_`, `IPTC`, and `JUMB`.
The encoder rejects core QSW2 tags through this generic array so there cannot
be two conflicting sources of color or pixel metadata.

Decode uses the v2 limits so metadata and decoded pixels have independent caps.

```c
qlic_decode_limits_v2 limits;
qlic_decode_limits_v2_default(&limits);
limits.max_metadata_bytes = 4 * 1024 * 1024;

qlic_hdr_image decoded = {0};
decoded.struct_size = sizeof(decoded);
int status = qlic_decode_hdr(data, size, &limits, &decoded);
if (status == QLIC_OK) {
  /* No conversion or tone mapping has occurred. */
  qlic_hdr_image_free(&decoded);
}
```

QSW2 is native QLIC framing around the existing CPU-native QSW1 byte-slice
payload. It does not embed PNG or another image container. The first version is
unsigned-integer only; half/float EXR bit preservation and bounded row bands are
future additive methods, not silent changes to this syntax. The complete
contract is in [hdr-format.md](hdr-format.md).

API version 7 appended the metadata array to `qlic_hdr_image` without changing
the version-1 wire grammar. A current decoder accepts the older, shorter image
struct for a QSW2 file with no ancillary blocks. If a file contains such
blocks, decoding into the old struct returns `QLIC_UNSUPPORTED_FORMAT` instead
of silently dropping them. `qlic_get_info_v2` reports `metadata_count` before
full decode.

API version 8 adds exact-region and validated-row delivery, progress callbacks,
and cooperative cancellation. It does not change any QLIC wire profile.

## Decode

```c
qlic_image image = {0};
int status = qlic_decode_rgba(data, size, NULL, &image);
if (status == QLIC_OK) {
  qlic_image_free(&image);
}
```

Pass null to use the default limits. To change them:

```c
qlic_decode_limits limits;
qlic_decode_limits_default(&limits);
limits.threads = qlic_hardware_thread_count();
limits.max_pixels = 16000000;

qlic_image image = {0};
int status = qlic_decode_rgba(data, size, &limits, &image);
```

The defaults are one thread, 512 MiB for input and payload data, 67,108,864 pixels, a 512 MiB animation-memory budget, and 100,000 frames. The animation budget includes decoded frame storage, frame bookkeeping, and temporary reconstruction memory.

### Caller-owned pixels

Use `qlic_decode_pixels` when an engine, browser, or asset loader already owns
the output allocation. It takes the complete v2 resource limits and never
transfers ownership.

```c
qlic_pixel_buffer output = {0};
output.struct_size = sizeof(output);
output.format = QLIC_PIXELS_RGBA8;
output.pixels = destination;
output.pixels_size = destination_size;
output.stride = destination_stride;

int status = qlic_decode_pixels(data, size, NULL, &output);
```

Packed RGBA output from the common native QST1 path is decoded directly into
the caller's allocation. Other current paths use a checked temporary and copy
the final rows; this is an implementation property, not a wire-format
distinction. RGB8 is accepted only when alpha is completely opaque. Gray8 is
accepted only when alpha is opaque and all three color values are equal, so
the function never silently discards alpha or color information. GrayA8 accepts
variable alpha but still requires equal color values. Available formats are
`QLIC_PIXELS_GRAY8`, `QLIC_PIXELS_GRAYA8`, `QLIC_PIXELS_RGB8`, and
`QLIC_PIXELS_RGBA8`. Animation,
wide integer, and HDR files return `QLIC_UNSUPPORTED_FORMAT` through this
still-image API.

`qlic_encode_pixels` takes the matching `qlic_pixel_input` descriptor, so a
caller can encode Gray8, GrayA8, RGB8, or RGBA8 without first building its own
RGBA buffer. The current encoder normalizes non-RGBA input internally; this is
an API convenience and does not create another wire representation.

### Regions, rows, progress, and cancellation

`qlic_decode_region_rgba` writes an exact RGBA8 rectangle into a
caller-owned `qlic_pixel_buffer`. `qlic_decode_rows_rgba` validates the entire
file and decoded-pixel checksum before invoking the row callback in raster
order. A row pointer is borrowed only for that callback.

```c
qlic_decode_observer observer = {0};
observer.struct_size = sizeof(observer);
observer.progress = update_progress; /* return zero to cancel */
observer.cancelled = is_cancelled;   /* return nonzero to cancel */
observer.user = job;

qlic_region crop = { .x = 100, .y = 80, .width = 640, .height = 480 };
qlic_pixel_buffer output = {0};
output.struct_size = sizeof(output);
output.format = QLIC_PIXELS_RGBA8;
output.pixels = crop_pixels;
output.pixels_size = crop_pixels_size;
output.stride = crop_stride;

int status = qlic_decode_region_rgba(data, size, &limits, &crop,
                                     &observer, &output);
```

Cancellation is cooperative and returns `QLIC_CANCELLED`. Checkpoints occur
before and after complete entropy validation and during row/region delivery.
Because native QST1 predictors are causal, arbitrary crops cannot skip the
preceding entropy state. Region decode therefore saves destination allocation
and copy bandwidth but does not promise proportional decode time or working
memory. The callback total is stable for one call, progress is monotonic, and
the final successful callback has `completed == total`.

## Animation

`qlic_encode_animation` takes an array of `qlic_frame_input`. Every frame has its own byte count, stride, dimensions, and delay. A zero delay is normalized to 100 milliseconds, and a loop count of zero means repeat indefinitely.

`qlic_decode_rgba` accepts still-image QLIC data. `qlic_decode_animation` accepts either still or animated QLIC data and returns a `qlic_animation`. Use `qlic_animation_free` when finished.

`qlic_get_info` reads dimensions, frame count, and animation state without decoding pixel data.

## Errors and ownership

Functions return `QLIC_OK` or a negative status. `qlic_status_string` describes the status. `qlic_last_error` gives details from the latest call on the current thread.

Memory returned by an encode is released with `qlic_free`. Decoded RGBA8 still
images use `qlic_image_free`, native-wide images use `qlic_wide_image_free`,
self-describing HDR images use `qlic_hdr_image_free`, and decoded animations use
`qlic_animation_free`.

Output pointers and structs must be fresh and initialized to zero. Before reusing a successful output object, release its existing storage with the matching QLIC free function; decode and encode calls clear their output object before validation and do not free caller-owned prior contents.

Input memory stays with the caller and has to remain valid until the call returns. Separate threads can call the SDK at the same time.

## ABI compatibility

Initialize option and limit structs with their matching default function.
`struct_size` allows a newer library to accept a larger caller struct. Unknown
flags and decode limit reserved fields must remain zero. `QLIC_API_VERSION`
identifies the public source/ABI contract, making it separated from the fileformat
compatibility promise.
