#define QLIC_NO_MAIN
#include "../codec/src/qlic.c"

static int empty_candidate_test(void) {
  uint8_t pixel[] = {17, 17, 17, 255};
  Image image = {1, 1, pixel};
  Candidate candidate = {0};
  Buf file = {0};
  int ok = 1;
  for (unsigned i = 0; i < 4u; ++i) {
    clear_err();
    candidate.compressed = i == 0u || i == 3u ? NULL : pixel;
    candidate.compressed_size = i < 2u ? 0u : 1u;
    candidate.payload_size = i == 0u || i == 2u ? 0u : 1u;
    if (mk_file(&image, &candidate, &file) || file.size || !g_err[0]) {
      fprintf(stderr, "empty candidate %u was serialized\n", i);
      ok = 0;
      break;
    }
  }
  buf_free(&file);
  return ok;
}

static uint32_t palette_random(uint32_t *state) {
  *state ^= *state << 13;
  *state ^= *state >> 17;
  *state ^= *state << 5;
  return *state;
}

static int palette_roundtrip(const char *layout, uint32_t height,
                             unsigned colors) {
  const uint32_t width = 4000u;
  const size_t pixels = (size_t)width * height;
  const size_t bytes = pixels * 4u;
  Image image = {width, height, (uint8_t *)malloc(bytes)};
  Image decoded = {0};
  Candidate candidate = {0};
  Buf file = {0};
  uint16_t pattern[512];
  uint8_t palette[257][4];
  uint32_t state = UINT32_C(0xb1834e29);
  int ok = 0;
  if (!image.rgba)
    goto done;
  for (unsigned i = 0; i < 512u; ++i)
    pattern[i] = (uint16_t)(i < colors ? i : palette_random(&state) % colors);
  for (unsigned i = 511u; i > 0u; --i) {
    unsigned j = palette_random(&state) % (i + 1u);
    uint16_t value = pattern[i];
    pattern[i] = pattern[j];
    pattern[j] = value;
  }
  int gray = strncmp(layout, "gray", 4u) == 0;
  int alpha = strcmp(layout, "rgba") == 0 || strcmp(layout, "graya") == 0;
  for (unsigned color = 0; color < colors; ++color) {
    uint8_t *p = palette[color];
    unsigned level = colors == 1u ? 17u : color * 255u / (colors - 1u);
    if (!gray && colors >= 2u && colors <= 8u) {
      unsigned bits = color & 1u ? 7u - (color >> 1u) : color >> 1u;
      p[0] = bits & 1u ? 255u : 0u;
      p[1] = bits & 2u ? 255u : 0u;
      p[2] = bits & 4u ? 254u : 1u;
    } else {
      p[0] = (uint8_t)level;
      p[1] = gray ? p[0] : (uint8_t)(color * 149u + 37u);
      p[2] = gray ? p[0] : (uint8_t)(255u - level);
    }
    p[3] = alpha ? (color & 1u ? 127u : 0u) : 255u;
  }
  for (size_t i = 0; i < pixels; ++i)
    memcpy(image.rgba + i * 4u, palette[pattern[i & 511u]], 4u);
  clear_err();
  uint32_t counted = 0;
  if ((colors > 1u && !looks_noisy(&image, scan_mode(&image), pixels)) ||
      !probe_palette_count(&image, 4096u, &counted) || counted != colors) {
    fprintf(stderr, "test image must exercise noisy palette input\n");
    goto done;
  }
  if (!enc_mem(&image, &file, &candidate) || !candidate.compressed ||
      file.size <= QLIC_HEADER_SIZE + QLIC_FOOTER_SIZE ||
      !dec_qlic(file.data, file.size, &decoded, NULL) ||
      decoded.width != width || decoded.height != height ||
      memcmp(image.rgba, decoded.rgba, bytes) != 0) {
    fprintf(stderr, "%s %ux%u round trip failed: %s\n", layout, width,
            height, qlic_core_error());
    goto done;
  }
  printf("%s %ux%u, %u colors: %zu bytes, exact RGBA round trip\n", layout,
         width, height, colors, file.size);
  ok = 1;
done:
  candidate_free(&candidate);
  buf_free(&file);
  image_free(&decoded);
  image_free(&image);
  return ok;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "empty") == 0)
    return empty_candidate_test() ? 0 : 1;
  if ((argc != 3 && argc != 4) ||
      (strcmp(argv[1], "rgb") && strcmp(argv[1], "rgba") &&
       strcmp(argv[1], "gray") && strcmp(argv[1], "graya")) ||
      (strcmp(argv[2], "4000") && strcmp(argv[2], "4001")))
    return 2;
  unsigned long colors = 7u;
  if (argc == 4) {
    char *end = NULL;
    colors = strtoul(argv[3], &end, 10);
    if (!argv[3][0] || *end || !colors || colors > 257u)
      return 2;
  }
  return palette_roundtrip(argv[1], strcmp(argv[2], "4000") ? 4001u : 4000u,
                           (unsigned)colors)
             ? 0
             : 1;
}
