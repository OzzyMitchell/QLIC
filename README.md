# QLIC

Quick Lossless Image Codec.

A lossless codec for still images, RGBA animation, and unsigned 8–24-bit integer
samples. Supports color profiles, alpha, and photographic metadata. Encoding is
automatic, with no effort or quality settings.

[Web demo](https://qlic.pages.dev/) · [Benchmarks](https://qlic.pages.dev/benchmarks/)

## CLI

Use `pack` to encode an image and `unpack` to decode it.

```text
qlic pack input.png output.qlic
qlic unpack input.qlic output.png
qlic info input.qlic --json
qlic verify input.qlic
```

`verify` checks a file without writing an image of any kind. Use `--threads N` or
`--threads all` to set CPU use.

Supported inputs include PNG, WebP, JPEG XL, TIFF, and BMP when their loaders
are available. Linux also supports AVIF. Windows can use installed WIC decoders,
including JPEG and GIF.

## Tools

- Windows app: `qlic-gui.exe`
- [C/C++ SDK](docs/sdk.md)
- [Rust decoder](rust/qlic-decoder)
- [WebAssembly](web/README.md)
- [Windows Explorer and WIC support](packaging/README-wic.md)

## Build

On Windows, with CMake 3.25+ and Visual Studio 2022+:

```powershell
.\build.ps1
```

On Debian or Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
  libpng-dev libwebp-dev libjxl-dev libavif-dev libtiff-dev libwim-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

For more detail: [color and HDR](docs/profiles.md), [file format](docs/format.md),
[architecture and limits](docs/architecture.md), [benchmark results](docs/benchmark-current.md),
and [support](SUPPORT.md).

Apache-2.0. The Rust LZMS port also retains its MIT license.
