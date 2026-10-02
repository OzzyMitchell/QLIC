# QLIC 1.0 benchmark current

The full development workspace keeps per-file rows, executable and library
hashes, source manifests, PGO input, timing rotations, and rejected
experiments.

## Corpus and method

The corpus contains 1,964,362,720 pixels. It combines DIV2K validation, CLIC
2022 validation, QOI benchmark categories, and a deterministic Enrico UI
selection. After exact normalization and duplicate removal it contains natural
photos, photographic and synthetic textures, game and web screenshots, mobile
interfaces, icons, transparent isolated objects, RGB, and RGBA. No production
route reads a filename, directory, dataset label, or category.

Every encoded result is decoded and compared byte-for-byte with the normalized
source. Encoders run on one pinned logical processor with rotated order for fairness. Encode
wall time includes startup and file I/O. Decode timing uses the exact retained
files, performs an untimed exact decode, then averages three pinned process
runs per file. Decode wall time includes process startup and output file I/O.
The machine was an AMD Ryzen 9 9950X3D running Windows 11. QLIC uses the
portable x86-64 Clang 22.1.4 Release build shipped here: no `-march=native` and
no PGO. The final release rerun covers QLIC, JPEG XL effort 9, and WebP preset
6.

## Encode

| Codec setting | Bytes | Seconds |
| --- | ---: | ---: |
| JPEG XL lossless effort 1 | 1,546,891,144 | 111.053 |
| JPEG XL lossless effort 2 | 1,411,868,992 | 195.489 |
| JPEG XL lossless effort 3 | 1,311,022,795 | 274.373 |
| JPEG XL lossless effort 4 | 1,278,581,912 | 497.141 |
| JPEG XL lossless effort 5 | 1,230,084,951 | 629.126 |
| JPEG XL lossless effort 6 | 1,214,206,451 | 754.620 |
| JPEG XL lossless effort 7 | 1,187,851,480 | 1,087.755 |
| JPEG XL lossless effort 8 | 1,171,971,513 | 2,977.934 |
| JPEG XL lossless effort 9 | 1,164,424,898 | 5,674.173 |
| QLIC automatic policy | 1,160,913,984 | 618.673 |
| WebP lossless preset 1 | 1,339,854,110 | 296.869 |
| WebP lossless preset 2 | 1,310,683,488 | 378.188 |
| WebP lossless preset 3 | 1,297,243,720 | 442.578 |
| WebP lossless preset 4 | 1,296,153,364 | 455.294 |
| WebP lossless preset 5 | 1,292,381,866 | 472.913 |
| WebP lossless preset 6 | 1,289,518,274 | 585.964 |
| WebP lossless preset 7 | 1,286,907,370 | 737.723 |
| WebP lossless preset 8 | 1,272,997,810 | 1,211.999 |
| WebP lossless preset 9 | 1,256,259,092 | 9,032.945 |
| OxiPNG level 6 | 1,618,637,169 | 9,105.634 |

QLIC is 3,510,914 bytes, or 0.301515%, smaller than JPEG XL effort 9. It is
128,604,290 bytes, or 9.973049%, smaller than WebP preset 6. QLIC is smaller
than JPEG XL effort 9 on 1,861 files, larger on 1,304, and tied on two.

The current QLIC total is the sum of all 3,167 retained per-image rows produced
by the published `qlic.exe`; every result decoded exactly. QLIC, JPEG XL effort
9, and WebP preset 6 were timed together in the final release campaign.

## Decode

| Codec setting | Seconds | MP/s | Time relative to QLIC |
| --- | ---: | ---: | ---: |
| QLIC automatic policy | 236.927 | 8.291 | baseline |
| JPEG XL effort 9 files | 307.602 | 6.386 | 29.83% slower |
| WebP preset 6 files | 117.887 | 16.663 | 50.24% faster |

QLIC is not the decode-speed winner. WebP is faster on this corpus.
QLIC uses 22.98% less decode time than JPEG XL effort 9 while producing a
slightly smaller total (On this corpus atleast). WebP preset 6 uses 50.24% less decode time than QLIC
while its files total 128,604,290 bytes more.

## Corpus variation

QLIC is often smaller on interfaces, compact textures, structured graphics,
repetition, and mixed screenshot assets. JPEG XL remains smaller on a subset of
photographic, screenshot, and alpha-heavy files. WebP can remain preferable
when decode latency dominates storage. The encoder therefore uses low-cost
content signals only to decide which bounded candidate to try, then
requires an exact completed-file win. It never forces a specialist from the
content label, and rejected trials remain part of the benchmark ledger so a
future optimization won't introduce them ideally.
