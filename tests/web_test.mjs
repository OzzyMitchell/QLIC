import { readFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { inflateSync } from "node:zlib";

if (process.argv.length < 9) {
  throw new Error("web_test.mjs expects module, wasm, still, animation, and LZMS paths");
}

const moduleUrl = pathToFileURL(process.argv[2]).href;
const { createQlic } = await import(moduleUrl);
const wasm = await readFile(process.argv[3]);
const stillBytes = await readFile(process.argv[4]);
const animationBytes = await readFile(process.argv[5]);
const qlic = await createQlic({ wasmBinary: wasm });

const originalModuleFetch = globalThis.fetch;
let wasmFetches = 0;
try {
  globalThis.fetch = async () => {
    wasmFetches++;
    return new Response(wasm, {
      headers: { "content-type": "application/octet-stream" }
    });
  };
  const fetchedQlic = await createQlic({
    wasmUrl: "https://qlic.invalid/qlic-web.wasm"
  });
  if (!fetchedQlic.validate(stillBytes) || wasmFetches !== 1) {
    throw new Error("Wasm MIME fallback repeated the network request");
  }
} finally {
  globalThis.fetch = originalModuleFetch;
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; ++bit)
      crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function decodeRgbaPng(bytes) {
  const signature = [137, 80, 78, 71, 13, 10, 26, 10];
  if (signature.some((value, index) => bytes[index] !== value))
    throw new Error("PNG signature differs");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const idat = [];
  let width = 0;
  let height = 0;
  let offset = 8;
  while (offset + 12 <= bytes.length) {
    const size = view.getUint32(offset);
    const typeOffset = offset + 4;
    const dataOffset = offset + 8;
    const end = dataOffset + size;
    if (end + 4 > bytes.length) throw new Error("PNG chunk exceeds input");
    const type = String.fromCharCode(...bytes.subarray(typeOffset, dataOffset));
    if (view.getUint32(end) !== crc32(bytes.subarray(typeOffset, end)))
      throw new Error(`PNG ${type} CRC differs`);
    if (type === "IHDR") {
      width = view.getUint32(dataOffset);
      height = view.getUint32(dataOffset + 4);
      if (size !== 13 || bytes[dataOffset + 8] !== 8 ||
          bytes[dataOffset + 9] !== 6) {
        throw new Error("PNG is not RGBA8");
      }
    } else if (type === "IDAT") {
      idat.push(bytes.subarray(dataOffset, end));
    } else if (type === "IEND") {
      break;
    }
    offset = end + 4;
  }
  const raw = inflateSync(Buffer.concat(idat.map(chunk => Buffer.from(chunk))));
  const rgba = new Uint8Array(width * height * 4);
  const rowBytes = width * 4;
  if (raw.length !== (rowBytes + 1) * height)
    throw new Error("PNG scanline size differs");
  for (let row = 0; row < height; row++) {
    const source = row * (rowBytes + 1);
    if (raw[source] !== 0) throw new Error("PNG used an unexpected filter");
    rgba.set(raw.subarray(source + 1, source + 1 + rowBytes), row * rowBytes);
  }
  return { width, height, rgba };
}

function makeModeTiles(nativeBytes, count) {
  if (nativeBytes.length <= 32 || count < 1)
    throw new Error("invalid native fixture for MODE_TILES");
  const nativeView = new DataView(
    nativeBytes.buffer, nativeBytes.byteOffset, nativeBytes.byteLength);
  const width = nativeView.getUint32(4, true);
  const tileHeight = nativeView.getUint32(8, true);
  const channels = nativeBytes[28 + 12];
  const chunk = nativeBytes.subarray(28, nativeBytes.length - 4);
  if (nativeBytes[12] !== 9 || (channels !== 1 && channels !== 3 && channels !== 4))
    throw new Error("fixture is not a supported native QST stream");
  const payloadSize = 4 + count * 4 + count * chunk.length;
  const file = new Uint8Array(28 + payloadSize + 4);
  const view = new DataView(file.buffer);
  file.set([0x51, 0x4c, 0x49, 0x43]);
  view.setUint32(4, width, true);
  view.setUint32(8, tileHeight * count, true);
  file[12] = 14;
  file[13] = 0;
  file[14] = channels;
  file[15] = 0x80;
  view.setUint32(16, tileHeight, true);
  view.setBigUint64(20, BigInt(payloadSize), true);
  let position = 28;
  view.setUint32(position, count, true);
  position += 4;
  for (let index = 0; index < count; ++index) {
    view.setUint32(position, chunk.length, true);
    position += 4;
  }
  for (let index = 0; index < count; ++index) {
    file.set(chunk, position);
    position += chunk.length;
  }
  view.setUint32(position, crc32(file.subarray(0, position)), true);
  return file;
}

const encodeWidth = 37;
const encodeHeight = 29;
const encodePixels = new Uint8Array(encodeWidth * encodeHeight * 4);
for (let y = 0; y < encodeHeight; ++y) {
  for (let x = 0; x < encodeWidth; ++x) {
    const offset = (y * encodeWidth + x) * 4;
    encodePixels[offset] = (x * 7 + y * 3) & 255;
    encodePixels[offset + 1] = (x ^ y) * 9 & 255;
    encodePixels[offset + 2] = (x * 5 + y * 11) & 255;
    encodePixels[offset + 3] = (x + y) % 7 ? 255 : 96;
  }
}
const encoded = qlic.encode(encodePixels, encodeWidth, encodeHeight);
if (encoded.length < 32 ||
    String.fromCharCode(...encoded.subarray(0, 4)) !== "QLIC") {
  throw new Error("WebAssembly encode produced an invalid container");
}
const encodedRoundtrip = qlic.decode(encoded);
if (encodedRoundtrip.width !== encodeWidth ||
    encodedRoundtrip.height !== encodeHeight ||
    encodedRoundtrip.frames.length !== 1 ||
    encodedRoundtrip.frames[0].rgba.length !== encodePixels.length) {
  throw new Error("WebAssembly encode dimensions did not roundtrip");
}
for (let i = 0; i < encodePixels.length; ++i) {
  if (encodedRoundtrip.frames[0].rgba[i] !== encodePixels[i])
    throw new Error(`WebAssembly encode changed pixel byte ${i}`);
}

const pngPixels = new Uint8Array([
  17, 33, 65, 0,
  255, 127, 1, 255,
  0, 254, 128, 19,
  91, 92, 93, 94
]);
const checkPng = async label => {
  const png = await qlic.encodePng(pngPixels, 2, 2);
  const image = decodeRgbaPng(png);
  if (image.width !== 2 || image.height !== 2 ||
      image.rgba.some((value, index) => value !== pngPixels[index])) {
    throw new Error(`${label} PNG export changed RGBA pixels`);
  }
};
await checkPng("compressed");
const savedCompressionStream = globalThis.CompressionStream;
try {
  globalThis.CompressionStream = undefined;
  await checkPng("fallback");
} finally {
  globalThis.CompressionStream = savedCompressionStream;
}

const still = qlic.decode(stillBytes);
if (still.width !== 64 || still.height !== 64 || still.frames.length !== 1 ||
    still.animated || still.frames[0].rgba.byteLength !== 64 * 64 * 4) {
  throw new Error("still image WebAssembly decode failed");
}
if (qlic.validate(stillBytes) !== true || qlic.validate(animationBytes) !== true)
  throw new Error("WebAssembly validation did not accept retained files");

const blobStill = await qlic.decodeBlob(new Blob([stillBytes]));
if (blobStill.width !== still.width || blobStill.height !== still.height ||
    await qlic.validateBlob(new Blob([stillBytes])) !== true) {
  throw new Error("Blob WebAssembly helpers failed");
}
const originalFetch = globalThis.fetch;
try {
  globalThis.fetch = async () => new Response(stillBytes, {
    headers: {"content-length": String(stillBytes.byteLength)}
  });
  const urlStill = await qlic.decodeUrl("https://qlic.invalid/still.qlic");
  if (urlStill.width !== still.width || urlStill.height !== still.height ||
      await qlic.validateUrl("https://qlic.invalid/still.qlic") !== true) {
    throw new Error("URL WebAssembly helpers failed");
  }
} finally {
  globalThis.fetch = originalFetch;
}

const damagedStill = new Uint8Array(stillBytes);
damagedStill[Math.floor(damagedStill.length / 2)] ^= 0x40;
let damagedRejected = false;
try {
  qlic.validate(damagedStill);
} catch {
  damagedRejected = true;
}
if (!damagedRejected)
  throw new Error("WebAssembly validation accepted damaged input");

const animation = qlic.decode(animationBytes);
if (animation.width !== 2 || animation.height !== 2 ||
    animation.frames.length !== 2 || !animation.animated ||
    animation.frames[0].delay !== 40 || animation.frames[1].delay !== 70) {
  throw new Error("animation WebAssembly decode failed");
}

const corrupt = new Uint8Array(stillBytes);
corrupt[corrupt.length - 1] ^= 1;
let rejected = false;
try {
  qlic.decode(corrupt);
} catch {
  rejected = true;
}
if (!rejected) throw new Error("corrupt WebAssembly input was accepted");

rejected = false;
try {
  qlic.decode(makeModeTiles(stillBytes, 257));
} catch {
  rejected = true;
}
if (!rejected)
  throw new Error("257-chunk WebAssembly MODE_TILES stream was accepted");

const lzmsCases = [
  [process.argv[6], 512, 512, 0xe1d7051e],
  [process.argv[7], 384, 256, 0x3bf96fd4],
  [process.argv[8], 257, 9, 0x10528ad2],
];
for (const [path, width, height, expectedCrc] of lzmsCases) {
  const decoded = qlic.decode(await readFile(path));
  if (decoded.width !== width || decoded.height !== height ||
      decoded.frames.length !== 1 ||
      crc32(decoded.frames[0].rgba) !== expectedCrc) {
    throw new Error(`LZMS WebAssembly decode failed: ${path}`);
  }
}

function planarPaletteValue(index, channel) {
  if (channel === 0) return index & 255;
  if (channel === 1) return index >>> 8;
  if (channel === 2) return (index * 37 + 11) & 255;
  return (index * 13 + 97) & 255;
}

function makePlanarCpal(layout) {
  const width = 17;
  const height = 17;
  const pixels = width * height;
  const paletteCount = 257;
  const paletteBytes = paletteCount * 4;
  const payloadSize = 1 + paletteBytes + pixels * 2;
  const file = new Uint8Array(28 + payloadSize + 4);
  const view = new DataView(file.buffer);
  file.set([0x51, 0x4c, 0x49, 0x43]);
  view.setUint32(4, width, true);
  view.setUint32(8, height, true);
  file[12] = 13;
  file[13] = 13;
  file[14] = 9;
  file[15] = 0x80;
  view.setUint32(16, paletteCount, true);
  view.setBigUint64(20, BigInt(payloadSize), true);
  let position = 28;
  file[position++] = layout;
  for (let channel = 0; channel < 4; ++channel) {
    let previous = 0;
    for (let index = 0; index < paletteCount; ++index) {
      const value = planarPaletteValue(index, channel);
      file[position++] = index ? (value - previous) & 255 : value;
      previous = value;
    }
  }
  const low = position;
  const high = low + pixels;
  for (let pixel = 0; pixel < pixels; ++pixel) {
    const index = pixel % paletteCount;
    if (layout === 0) {
      file[position++] = index & 255;
      file[position++] = index >>> 8;
    } else {
      file[low + pixel] = index & 255;
      file[high + pixel] = index >>> 8;
    }
  }
  if (layout === 1) position += pixels * 2;
  if (position !== 28 + payloadSize)
    throw new Error("internal planar CPAL fixture size mismatch");
  view.setUint32(position, crc32(file.subarray(0, position)), true);
  return file;
}

for (let layout = 0; layout < 2; ++layout) {
  const decoded = qlic.decode(makePlanarCpal(layout));
  if (decoded.width !== 17 || decoded.height !== 17 ||
      decoded.frames.length !== 1)
    throw new Error(`planar CPAL layout ${layout} dimensions differ`);
  const rgba = decoded.frames[0].rgba;
  for (let pixel = 0; pixel < 17 * 17; ++pixel) {
    const index = pixel % 257;
    for (let channel = 0; channel < 4; ++channel) {
      if (rgba[pixel * 4 + channel] !==
          planarPaletteValue(index, channel)) {
        throw new Error(`planar CPAL layout ${layout} pixel differs`);
      }
    }
  }
}

const optionalPaths = process.argv.slice(9);
const tileMarker = optionalPaths.indexOf("--mode-tiles-rgba");
let mode45Path = null;
if (tileMarker >= 0) {
  if (tileMarker + 1 >= optionalPaths.length)
    throw new Error("--mode-tiles-rgba requires a fixture path");
  mode45Path = optionalPaths[tileMarker + 1];
  optionalPaths.splice(tileMarker, 2);
}
const wideMarker = optionalPaths.indexOf("--wide");
let widePaths = [];
if (wideMarker >= 0) {
  if (wideMarker + 4 >= optionalPaths.length)
    throw new Error("--wide requires four fixture paths");
  widePaths = optionalPaths.slice(wideMarker + 1, wideMarker + 5);
  optionalPaths.splice(wideMarker, 5);
}
const hdrMarker = optionalPaths.indexOf("--hdr");
let hdrPath = null;
if (hdrMarker >= 0) {
  if (hdrMarker + 1 >= optionalPaths.length)
    throw new Error("--hdr requires a fixture path");
  hdrPath = optionalPaths[hdrMarker + 1];
  optionalPaths.splice(hdrMarker, 2);
}
const hlgMarker = optionalPaths.indexOf("--hlg");
let hlgPath = null;
if (hlgMarker >= 0) {
  if (hlgMarker + 1 >= optionalPaths.length)
    throw new Error("--hlg requires a fixture path");
  hlgPath = optionalPaths[hlgMarker + 1];
  optionalPaths.splice(hlgMarker, 2);
}
const describedMarker = optionalPaths.indexOf("--described8");
let describedPath = null;
if (describedMarker >= 0) {
  if (describedMarker + 1 >= optionalPaths.length)
    throw new Error("--described8 requires a fixture path");
  describedPath = optionalPaths[describedMarker + 1];
  optionalPaths.splice(describedMarker, 2);
}
const rejectMarker = optionalPaths.indexOf("--reject");
const decodePaths = rejectMarker < 0
  ? optionalPaths
  : optionalPaths.slice(0, rejectMarker);
const rejectPaths = rejectMarker < 0
  ? []
  : optionalPaths.slice(rejectMarker + 1);

if (widePaths.length) {
  const expected = [
    [3, 2, 1, 10, [0, 1, 511, 512, 1022, 1023]],
    [2, 2, 4, 16, [
      0, 1, 65534, 65535, 0x1234, 0x5678, 0x9abc, 0xdef0,
      65535, 0, 32768, 1, 42, 4242, 60000, 32767
    ]],
    [3, 2, 1, 17, [0, 1, 65535, 65536, 131070, 131071]],
    [2, 2, 3, 24, [
      0, 1, 0xffffff, 0x123456, 0xabcdef, 0x800000,
      0xfffffe, 0x010203, 0xfedcba, 0x00ff00, 0xff0000, 0x0000ff
    ]]
  ];
  for (let index = 0; index < expected.length; ++index) {
    const wideBytes = await readFile(widePaths[index]);
    if (qlic.validate(wideBytes) !== true)
      throw new Error(`wide WebAssembly validation failed: ${widePaths[index]}`);
    const decoded = qlic.decodeWide(wideBytes);
    const [width, height, channels, bits, samples] = expected[index];
    if (decoded.width !== width || decoded.height !== height ||
        decoded.channels !== channels || decoded.bitsPerSample !== bits ||
        decoded.samples.length !== samples.length ||
        decoded.stride !== width * channels * (bits <= 16 ? 2 : 4))
      throw new Error(`wide WebAssembly shape differs: ${widePaths[index]}`);
    for (let sample = 0; sample < samples.length; ++sample) {
      if (decoded.samples[sample] !== samples[sample])
        throw new Error(`wide WebAssembly sample differs: ${widePaths[index]}:${sample}`);
    }
  }
}

if (hdrPath) {
  const hdrBytes = await readFile(hdrPath);
  if (qlic.validate(hdrBytes) !== true)
    throw new Error("HDR WebAssembly validation failed");
  const decoded = qlic.decodeHdr(hdrBytes);
  const expectedSamples = [
    0, 1, 4095, 4095, 4095, 2048, 1024, 3000,
    17, 255, 256, 1, 0x123, 0x456, 0x789, 0xabc
  ];
  const expectedIcc = [
    0, 0, 0, 16, 0x61, 0x63, 0x73, 0x70,
    0x51, 0x4c, 0x49, 0x43, 0x20, 0x26, 0x08, 0x14
  ];
  if (decoded.pixels.width !== 2 || decoded.pixels.height !== 2 ||
      decoded.pixels.channels !== 4 || decoded.pixels.bitsPerSample !== 12 ||
      decoded.sampleType !== "uint" || decoded.alphaAssociation !== "straight" ||
      decoded.colorAuthority !== "icc-preferred" || !decoded.cicp ||
      decoded.cicp.colorPrimaries !== 9 ||
      decoded.cicp.transferCharacteristics !== 16 ||
      decoded.cicp.matrixCoefficients !== 0 || !decoded.cicp.fullRange ||
      !decoded.masteringDisplay || decoded.masteringDisplay.maxLuminance !== 10000000 ||
      decoded.masteringDisplay.minLuminance !== 50 || !decoded.contentLight ||
      decoded.contentLight.maxContentLightLevel !== 1000 ||
      decoded.contentLight.maxFrameAverageLightLevel !== 400)
    throw new Error("HDR WebAssembly metadata differs");
  for (let index = 0; index < expectedSamples.length; ++index) {
    if (decoded.pixels.samples[index] !== expectedSamples[index])
      throw new Error(`HDR WebAssembly sample differs: ${index}`);
  }
  if (!decoded.icc || decoded.icc.length !== expectedIcc.length)
    throw new Error("HDR WebAssembly ICC length differs");
  for (let index = 0; index < expectedIcc.length; ++index) {
    if (decoded.icc[index] !== expectedIcc[index])
      throw new Error(`HDR WebAssembly ICC differs: ${index}`);
  }
}

if (hlgPath) {
  const bytes = await readFile(hlgPath);
  if (qlic.validate(bytes) !== true)
    throw new Error("HLG WebAssembly validation failed");
  const decoded = qlic.decodeHdr(bytes);
  const expected = [
    0, 1, 1023, 1023, 512, 256, 17, 511, 1000, 64, 900, 333
  ];
  if (decoded.pixels.width !== 2 || decoded.pixels.height !== 2 ||
      decoded.pixels.channels !== 3 || decoded.pixels.bitsPerSample !== 10 ||
      decoded.sampleType !== "uint" || decoded.alphaAssociation !== "none" ||
      decoded.colorAuthority !== "cicp" || decoded.icc || !decoded.cicp ||
      decoded.cicp.colorPrimaries !== 9 ||
      decoded.cicp.transferCharacteristics !== 18 ||
      decoded.cicp.matrixCoefficients !== 0 || !decoded.cicp.fullRange ||
      decoded.masteringDisplay || decoded.contentLight)
    throw new Error("HLG WebAssembly metadata differs");
  for (let index = 0; index < expected.length; ++index) {
    if (decoded.pixels.samples[index] !== expected[index])
      throw new Error(`HLG WebAssembly sample differs: ${index}`);
  }
}

if (describedPath) {
  const bytes = await readFile(describedPath);
  if (qlic.validate(bytes) !== true)
    throw new Error("8-bit self-describing WebAssembly validation failed");
  const decoded = qlic.decodeHdr(bytes);
  const expected = [0, 1, 255, 17, 127, 254, 255, 128, 2, 33, 66, 99];
  if (decoded.pixels.width !== 2 || decoded.pixels.height !== 2 ||
      decoded.pixels.channels !== 3 || decoded.pixels.bitsPerSample !== 8 ||
      decoded.sampleType !== "uint" || decoded.alphaAssociation !== "none" ||
      decoded.colorAuthority !== "cicp" || decoded.icc || !decoded.cicp ||
      decoded.cicp.colorPrimaries !== 1 ||
      decoded.cicp.transferCharacteristics !== 13 ||
      decoded.cicp.matrixCoefficients !== 0 || !decoded.cicp.fullRange ||
      decoded.masteringDisplay || decoded.contentLight)
    throw new Error("8-bit self-describing WebAssembly metadata differs");
  for (let index = 0; index < expected.length; ++index) {
    if (decoded.pixels.samples[index] !== expected[index])
      throw new Error(`8-bit self-describing WebAssembly sample differs: ${index}`);
  }
}

for (const path of decodePaths) {
  const decoded = qlic.decode(await readFile(path));
  if (!decoded.width || !decoded.height || !decoded.frames.length)
    throw new Error(`additional WebAssembly decode failed: ${path}`);
}

for (const path of rejectPaths) {
  let unsupported = false;
  try {
    qlic.decode(await readFile(path));
  } catch {
    unsupported = true;
  }
  if (!unsupported)
    throw new Error(`unsupported WebAssembly stream was accepted: ${path}`);
}

if (mode45Path) {
  const nativeMode45 = await readFile(mode45Path);
  const nativeDecoded = qlic.decode(nativeMode45);
  const tiledBytes = makeModeTiles(nativeMode45, 2);
  const tiledDecoded = qlic.decode(tiledBytes);
  const source = nativeDecoded.frames[0].rgba;
  const tiled = tiledDecoded.frames[0].rgba;
  if (tiledDecoded.width !== nativeDecoded.width ||
      tiledDecoded.height !== nativeDecoded.height * 2 ||
      tiled.length !== source.length * 2)
    throw new Error("RGBA mode45 MODE_TILES dimensions differ");
  for (let offset = 0; offset < source.length; ++offset) {
    if (tiled[offset] !== source[offset] ||
        tiled[offset + source.length] !== source[offset])
      throw new Error(`RGBA mode45 MODE_TILES byte differs at ${offset}`);
  }

  /* dec_tile allocates the full output before validating each chunk.  A bad
     first size with a repaired outer CRC exercises that post-allocation error
     path.  Repetition is intentional: a leaked 8 MiB output would exhaust the
     WebAssembly heap well before this loop finishes. */
  const malformed = new Uint8Array(tiledBytes);
  const malformedView = new DataView(malformed.buffer);
  malformedView.setUint32(32, 0xffffffff, true);
  malformedView.setUint32(
    malformed.length - 4,
    crc32(malformed.subarray(0, malformed.length - 4)), true);
  for (let attempt = 0; attempt < 144; ++attempt) {
    let rejected = false;
    try {
      qlic.decode(malformed);
    } catch {
      rejected = true;
    }
    if (!rejected)
      throw new Error("malformed RGBA MODE_TILES stream was accepted");
  }
}

// Cross the large-image threshold with noisy palette data and hidden RGB.
{
  const width = 4000;
  const height = 4001;
  const pattern = new Uint8Array(256 * 4);
  let state = 0xb1834e29;
  for (let i = 0; i < 256; ++i) {
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    const color = (state >>> 0) % 7;
    pattern.set([
      color & 1 ? 255 : 0,
      color & 2 ? 255 : 0,
      color & 4 ? 255 : 0,
      color >= 4 ? (color === 4 ? 0 : 127) : 255
    ], i * 4);
  }
  const source = new Uint8Array(width * height * 4);
  for (let offset = 0; offset < source.length; offset += pattern.length)
    source.set(pattern.subarray(0, source.length - offset), offset);
  const encoded = qlic.encode(source, width, height);
  if (encoded.length <= 32)
    throw new Error("large palette input produced an empty QLIC file");
  const decoded = qlic.decode(encoded);
  if (decoded.width !== width || decoded.height !== height ||
      decoded.frames.length !== 1 ||
      !Buffer.from(decoded.frames[0].rgba).equals(Buffer.from(source)))
    throw new Error("large palette RGBA round trip differs");
}

console.log("QLIC WebAssembly tests passed");
