# QLIC file format

A still image decodes to RGBA pixels. An animation decodes to RGBA frames with millisecond delays.

All multi-byte integers in QLIC, QST1, QAN1/QAN2, QSW1, and QSW2 are
little-endian unless a field explicitly says otherwise. Every CRC is
CRC-32/ISO-HDLC: reflected polynomial `0xedb88320`, initial value
`0xffffffff`, final xor `0xffffffff`, and check value `0xcbf43926` for
`"123456789"`.

The QLIC 1.0 compatibility surface is split into explicit profiles in
[profiles.md](profiles.md). The normative Core Still container and QST1 fixed
grammar are in [core-still.md](core-still.md). This file also documents legacy
and extension syntax retained by the canonical decoder.

## Container

Every file starts with this 28 byte header.

```text
0   4   magic: QLIC
4   4   width
8   4   height
12  1   mode
13  1   transform
14  1   index bits or mode parameter
15  1   codec plus integrity flag
16  4   palette count, tile height, channel count, or frame count
20  8   uncompressed payload size
```

The high bit of byte 15 means a CRC32 follows the body. It covers the header and body together.

## Still images

Still image modes cover grayscale, grayscale with alpha, RGB, RGBA, palettes, separable rows, native streams, filtered streams, tiled streams, reversible tile transforms, and block programs.

Mode 18 contains block programs.

QBL1 stores commands in row order. A command can hold raw pixels, a flat color, a copy from the left or above, a small color map, or a 2 by 2 pattern map.

QBR1 adds exact copies from any earlier block. Backward distance is an unsigned base 128 varint measured in scan order blocks. The encoder keeps QBL1 when no arbitrary reference makes the file smaller.

QCF1 stores a 64 by 64 coordinate field and one exact folded residual stream. Each region stores five 4 bit coordinates in three bytes. The decoder derives flat, gradient, left, up, causal, and linear bases from decoded borders. Those bases are blended before the residual is applied.

QBL2 stores block predictors in row order and one exact folded residual stream. Predictors include zero, flat, a bilinear corner gradient, copies from the left or above with byte deltas, causal Paeth prediction, and PDM predictor coordinates packed as five 4 bit values.

QPD1 stores one PDM coordinate model for each 64 by 64 region and one exact folded residual stream. Decoding takes one pass after reading the residual stream.

The outer header for a native stream always uses the identity transform. QST1 stores the actual predictor mode, color transform, tile setting, and adaptation rate. Container codec store only means QST1 is stored directly. It does not mean the pixels or residuals are raw.

QST1 stores the adaptive range coder update rate in byte 17 when that byte is not holding a constant alpha value. Zero selects the default. Values 4 and 6 select the faster and slower rates. Every other value is invalid.

QST1 flag bits 2 through 4 can store an exact grayscale or RGB sample depth from 1 through 7. The encoder only uses this when every affected value belongs to that depth's full range scaling grid and the complete stream is smaller. The decoder rejects samples outside the stated range, restores them to 8 bit values, then verifies the pixel checksum.

Native mode 41 uses the mode 37 residual syntax with causal predictor map coding. Predictor tiles are visited in raster order. A tile first signals whether it differs from the tile on its left. When it differs, it signals against the tile above if that tile exists and has another predictor. A zero bit selects the referenced predictor. Otherwise, a 5 bit adaptive tree stores the predictor. Left and up decisions use separate probability sets with four contexts for predictor IDs 0, 1 through 4, 5 through 10, and 11 through 31.

Native mode 42 stores the same predictor in independent 128 row bands for each plane. The payload starts with little endian chunk sizes followed by range coded band chunks. The bands decode independently and are joined by the reversible color transform.

Native modes 43 through 54 keep the mode 37 residual and predictor map syntax while adding richer residual contexts. Mode 43 carries causal state between transformed color planes. Mode 44 adds local zero and sign state. Mode 45 adds spatial residual state and can omit the predictor map when the tile log is zero. Modes 46 through 50 add child, coarse, full coarse, root, and slower root probability levels. Mode 51 adds sign state for each predictor. Mode 52 conditions sign state on residual magnitude. Mode 53 refines zero, magnitude, and sign mixtures with update rates based on magnitude. Mode 54 retains those refined contexts and adds the causal weighted predictor as predictor-map entry 31. Every finalized mode has a distinct identifier and decoding behavior.

Mode 54 remains part of the stable decoder syntax, but the ordinary encoder no
longer selects it, as Its approximately one-percent retained size gain cost about
thirty percent in measured decode time. Forced benchmark trials and an explicit
stream-trace opt-in keep the historical encoder experiment without charging
ordinary files.

Native transforms 11 through 28 use reversible weighted red and green prediction for the blue plane. Transforms 29 through 34 add a reversible luma lift around the same red and green difference. Transform 35 uses red as the anchor and predicts blue from the red and green average. Transforms 36 and 37 use blue as the anchor and a fixed 40/24 blend to predict green from red/blue or red from green/blue. Transform 38 predicts the blue channel from a quadratic red/green tangent-space relation. Transform 39 uses a bounded integer approximation of the spherical tangent-space relation. Transform 40 uses the same spherical predictor while storing green in its native eight-bit range. All three are documented in `predictor-math.md`. Native decoders accept transform identifiers through 40.

Transforms 38 through 40 are additive syntax extensions. Current decoders continue to read every earlier native stream. Earlier decoders reject an unknown transform at the existing transform-range check instead of interpreting it as another transform.

Every static path produces exact RGBA pixels. Alpha is stored as data and is not composited.

Outer transform 11 is the blue-delta planar MED representation. It is valid
only for RGB or RGBA with the LZMS outer codec. The uncompressed payload stores
whole byte planes in this order:

```text
B
R - B
G - B
alpha, for RGBA only
```

Each plane contains modulo-256 residuals from the causal median-edge predictor
described in `predictor-math.md`. Planes are reconstructed in place before the
blue deltas are added back and pixels are interleaved. The payload is exactly
`width * height * channels` bytes. The current encoder considers this syntax
only after a completed RGB/RGBA byte-stream incumbent, never after a native or
palette incumbent; an XPRESS-Huffman result within 16% gates one LZMS trial,
and the completed file must be strictly smaller. Earlier decoders reject outer
transform 11 at their transform-range check.

Outer transform 12 is the tile-local palette representation. It is valid only
for mode 13 (CPAL). Header byte 14 stores the bit width for the complete global
palette, header bytes 16 through 19 store its color count, and the payload is:

```text
1 byte              tile size log2, 3..6
4 * palette_count   global RGBA palette entries
for each tile in raster order:
  varint             local palette count minus one
  local_count varints
                     sorted global palette IDs as first ID, then gap minus one
  packed indexes     tile_pixels * pal_bits(local_count), rounded to bytes
```

Packed indexes are least-significant-bit first. `pal_bits(n)` uses widths 1,
2, 4, and 8 for palette counts through 2, 4, 16, and 256, then the minimum
integer width from 9 through 16. Edge tiles use their actual clipped dimensions.
Sorted global IDs make every local palette
unambiguous; indexes outside the local palette, incomplete tiles, invalid
varints, and trailing bytes are corrupt. The current encoder bounds its search
to tile logs 3 through 6, a 65,536-entry global palette, and a 2 MiB candidate
payload, then emits transform 12 only under LZMS when its completed file is
strictly smaller. When the incumbent is already CPAL, its uncompressed payload
is also an upper bound on the tile payload; this prevents a marginal compressed
win from increasing decode work. The decoder also accepts the stored outer
codec so small conformance and robustness fixtures can exercise the syntax
without an LZMS dependency. Earlier decoders reject transform 12 at their
transform-range check.

Outer transform 13 is the channel-planar palette representation. It is valid
only for mode 13 (CPAL), palette counts from 257 through 65,536, and the exact
`pal_bits(palette_count)` header width. The payload is:

```text
1 byte                  index layout: 0 interleaved, 1 split
4 * palette_count       R plane, G plane, B plane, alpha plane
2 * width * height      fixed-width palette-ID bytes
```

Within each palette channel the first byte is absolute. Every later byte is a
wrapping modulo-256 delta from the previous palette entry. Layout 0 stores each
16-bit ID low byte then high byte. Layout 1 stores all low bytes followed by
all high bytes. The payload size is exact; other layout values, an index beyond
the palette, trailing data, and truncated data are corrupt.

The current encoder considers this representation only beside an already
completed CPAL incumbent. It rejects an uncompressed candidate more than 60%
larger than the incumbent payload, ranks both layouts with XPRESS-Huffman,
allows one LZMS trial only for the best layout within 10% of the completed
incumbent, and still requires a strictly smaller completed file. The
decoder also accepts the stored codec for conformance and robustness fixtures.
Earlier decoders reject transform 13 at their transform-range check.

Outer mode 14 stores horizontal bands as independently checked QST1 streams.
The outer transform is identity, header byte 14 is the channel count (1, 3, or
4), and header bytes 16 through 19 store the nonzero band height. Its decoded
payload is:

```text
4 bytes              little-endian band count
4 * count bytes      little-endian QST1 byte lengths
...                  consecutive QST1 band streams
```

The count must equal `ceil(height / band_height)`. Every nested stream must
decode to the outer width, the clipped height of that band, and the stated
channel count; extra or missing payload bytes are corrupt. The wire syntax caps
the table at 65,536 entries. The default decoder resource policy is stricter
and accepts at most 256 bands, failing with a resource-limit status before
allocating the task table. Implementations may expose a different checked
policy limit without changing the syntax.

The current encoder uses this syntax only for two bounded cases: the retained
legacy very-large-image fallback and a narrow RGBA native mode-45/transform-2
retry. The latter requires at least three million pixels, a height at least
twice the width, a fixed vertical-stationarity probe, no more than 256 bands,
and at least 8 KiB of completed-file saving. These are encoder policy rules,
not additional decoder validity requirements.

Predictor formulas and reversible transforms are in predictor-math.md.

## Native-wide integer samples

Mode 19 stores one, three, or four interleaved unsigned integer channels at a
shared precision from 9 through 24 bits per sample. It is additive: all older
still and animation modes keep their existing syntax, while an older decoder
rejects mode 19 rather than interpreting it as RGBA8.

The mode-19 outer header uses its mode parameters as follows:

```text
13  identity transform
14  bits per sample, 9..24
15  store codec plus integrity flag
16  channel count, 1, 3, or 4
20  QSW1 payload size
```

The outer payload begins with QSW1:

```text
0   4   marker: QSW1
4   1   method: 0 for byte slices
5   1   bits per sample
6   1   channel count
7   1   slice count: ceil(bits / 8), therefore 2 or 3
8   4   canonical sample CRC32
12  4   reserved zero
16  8*N little-endian QST1 slice lengths
...     N consecutive QST1 byte-slice streams
```

Slice zero contains the least significant byte of every sample in raster,
pixel, then channel order. Later slices contain the next byte. Unused high
bits in the final slice must be zero. Each slice is an ordinary QST1 image
with the outer width, height, and channel count, so the wide decoder reuses the
same optimized byte decoder instead of maintaining a second wide arithmetic
coder.

The canonical sample checksum visits the same raster/pixel/channel order and
feeds the least significant `ceil(bits / 8)` bytes of each numeric sample into
CRC32. This definition is independent of host endianness. Native APIs expose
9..16-bit samples as native-endian `uint16_t` and 17..24-bit samples as
native-endian `uint32_t`; those in-memory layouts are not copied verbatim to
the file.

## Self-described HDR and wide color

Mode 20 wraps the same exact QSW1 integer sample payload in QSW2 and associates
it with alpha semantics plus ICC and/or CICP color description. Optional MDCV
and CLLI chunks preserve mastering-display and content-light metadata. Unknown
ancillary chunks are preserved in order and returned byte-for-byte by current
decoders; older version-1 decoders may skip them. Unknown critical chunks are
rejected. The codec
does not implicitly convert color, premultiply alpha, or tone-map. The complete
versioned syntax and validation rules are in [hdr-format.md](hdr-format.md).

## Animation

Animation uses mode 17. Older QAN1 files are still decoded. Its four byte marker is followed by the frame count and loop count. Each frame stores:

```text
delay_ms
flags
nested_qlic_size
nested_static_qlic_bytes
```

Each frame is an independent still QLIC image. Nested animation is invalid.

QAN2 starts with the same frame count and loop count. Every frame starts with its delay and one of these types:

```text
0  key frame: nested_qlic_size, nested_static_qlic_bytes
1  duplicate: no additional data
2  rectangle: x, y, width, height, nested_qlic_size, nested_static_qlic_bytes
3  move: source_x, source_y, destination_x, destination_y, width, height, clear_rgba
```

The first frame has to be a key frame. A duplicate copies the previous frame. A rectangle copies the previous frame and replaces one region with the nested image. A move clears the source rectangle with one RGBA color, then copies pixels from the unchanged previous frame to the destination. Nested image dimensions have to match the key frame or rectangle exactly.

## Integrity

The container checksum covers the header, palette, compressed payload, and animation table. Native streams also store a stream checksum and a pixel checksum. Native-wide streams add a canonical full-precision sample checksum around the independently checked QST1 byte slices. The decoder requires and checks all of them.
