# QLIC WebAssembly

The Web package is an importable browser module. It doesn't need any sort of QLIC executable,
DLL, or local service.

Keep `qlic-web.js` and `qlic-web.wasm` together on an HTTP(S) host.
`qlic-web.d.ts` contains TypeScript declarations, and `qlic-worker.js` shows a
module-worker integration.

The built `index.html` is a self contained offline encoder and viewer. It
opens QLIC files, renders stills and animation, inspects pixels, exports PNG,
and supports wheel, drag, and pinch gestures. It is responsive on desktop and
mobile and works through `file://` where the browser permits local pages. Use
HTTP(S) for the reusable ES module because browsers restrict loose modules
loaded from `file://`.

## Use

```html
<canvas id="image"></canvas>
<script type="module">
  import { createQlic } from "./qlic-web.js";

  const qlic = await createQlic();
  const image = await qlic.decodeUrl("/images/picture.qlic");
  qlic.draw(image, document.querySelector("#image"));
</script>
```

The loader finds `qlic-web.wasm` beside the module. `wasmUrl` or `wasmBinary`
can override it. A local package can also be installed with:

```sh
npm install ./vendor/qlic-web
```

## Decode

`decode(bytes)`, `decodeBlob(file)`, and `decodeUrl(url)` return:

```js
{
  width,
  height,
  frames: [{ width, height, delay, rgba }],
  loopCount,
  animated
}
```

`validate`, `validateBlob`, and `validateUrl` fully decode without retaining
pixels. `draw` renders a frame; `play` presents animation.

## Encode

```js
const encoded = qlic.encode(rgba, width, height);
```

`rgba` must contain exactly `width * height * 4` bytes. The returned
`Uint8Array` owns its data. Encoding may run in a module worker.

To create an RGBA8 PNG without any canvas conversion:

```js
const png = await qlic.encodePng(rgba, width, height);
```

This keeps RGB values under zero alpha. The offline viewer uses it for the
current still or animation frame.

## Wide, HDR, and metadata

```js
const wide = qlic.decodeWide(wideBytes);
const hdr = qlic.decodeHdr(hdrBytes);
```

Wide output is a `Uint16Array` for 9--16 bits or `Uint32Array` for 17--24
bits. HDR output also carries ICC, CICP, mastering-display, content-light,
alpha, and opaque EXIF/XMP/IPTC/JUMBF metadata. QLIC does not tone-map, convert, interpret or otherwise mess with
those records. Rec. 2100 PQ and HLG are returned with transfer
characteristics 16 and 18 respectively; saved fixtures will verify both metadata
and exact samples.

## Limits

Defaults are 256 MiB input/payload, 33,554,432 pixels per frame, 4,096 frames,
256 MiB animation output, and 16 MiB HDR metadata. Lower them at the product
boundary when needed.

Browser image input accepts formats the browser can decode and renders them to
RGBA8. Lossy JPEG and WebP sources show a size warning. This input path
cannot preserve source precision, metadata, animation, or hidden RGB under zero
alpha. QLIC input decodes animation and exact RGBA8. Wide and HDR QLIC files use
a clearly labeled 8-bit SDR preview; the API still returns their exact samples
and metadata.

The responsive page is QLIC's current mobile surface. A native iOS or Android
port is future work.

## Build

LLVM and Node.js are required:

```powershell
.\scripts\build-web.ps1
```

The build checks the Wasm ABI, JavaScript API, and offline page. Release
validation also runs the packaged module and offline page in a browser.
