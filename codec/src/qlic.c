#include "qlic_core.h"
#include "lzms.h"
#include "parallel.h"
#include "stream.h"
#ifndef QLIC_NO_MAIN
#include "input.h"
#endif
#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <compressapi.h>
#include <shellapi.h>
#elif !defined(QLIC_WASM)
#include <unistd.h>
#endif
#ifdef QLIC_HAVE_WIMLIB
#include <wimlib.h>
#ifndef QLIC_WIMLIB_LZMS_LEVEL
#define QLIC_WIMLIB_LZMS_LEVEL 50
#endif
#endif
#include <stdarg.h>
#include <stdint.h>
#ifdef QLIC_WASM
#include <stddef.h>
#include <limits.h>
void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t s);
void *realloc(void *p, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
static int qlic_abs(int value) { return value < 0 ? -value : value; }
#define abs qlic_abs
#else
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#if defined(QLIC_STREAM_TRACE) && !defined(_WIN32)
#include <time.h>
#endif
#endif

#if defined(_WIN32) && !defined(ALL_PROCESSOR_GROUPS)
#define ALL_PROCESSOR_GROUPS 0xffff
#endif

typedef struct {
  uint32_t key;
  uint32_t value;
  uint8_t used;
} MapEntry;

/* API calls may run together, error and encoder state cannot be process globals
 */
#if defined(_MSC_VER)
__declspec(thread) static char g_err[1024];
#elif defined(__STDC_NO_THREADS__)
static char g_err[1024];
#else
static _Thread_local char g_err[1024];
#endif
#if defined(_MSC_VER)
__declspec(thread) static QlicCoreStatus g_status;
#elif defined(__STDC_NO_THREADS__)
static QlicCoreStatus g_status;
#else
static _Thread_local QlicCoreStatus g_status;
#endif
#if defined(_MSC_VER)
__declspec(thread) static unsigned g_threads = 1;
#elif defined(__STDC_NO_THREADS__)
static unsigned g_threads = 1;
#else
static _Thread_local unsigned g_threads = 1;
#endif
static const QlicDecodeLimits g_default_decode_limits = {
    QLIC_DEFAULT_MAX_FILE_BYTES,    QLIC_DEFAULT_MAX_PAYLOAD_BYTES,
    QLIC_DEFAULT_MAX_PIXELS,        QLIC_DEFAULT_MAX_ANIMATION_BYTES,
    QLIC_DEFAULT_MAX_DECODED_BYTES, QLIC_DEFAULT_MAX_METADATA_BYTES,
    QLIC_DEFAULT_MAX_FRAMES,        QLIC_DEFAULT_MAX_CHUNKS};

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
static int qlic_tile_palette_enabled(void) {
#ifdef QLIC_STREAM_TRACE
  const char *value = getenv("QLIC_DISABLE_TILE_PALETTE");
  return !(value && value[0] && value[0] != '0');
#else
  return 1;
#endif
}

static int qlic_tile_palette_middle_scales_enabled(void) {
#ifdef QLIC_STREAM_TRACE
  const char *value = getenv("QLIC_DISABLE_TILE_PALETTE_MIDDLE_SCALES");
  return !(value && value[0] && value[0] != '0');
#else
  return 1;
#endif
}

/* Retain a trace-build kill switch so the accepted route can be measured
   against the immediately preceding encoder without maintaining two trees. */
static int qlic_cpal_planar_enabled(void) {
#ifdef QLIC_STREAM_TRACE
  const char *value = getenv("QLIC_DISABLE_CPAL_PLANAR");
  return !(value && value[0] && value[0] != '0');
#else
  return 1;
#endif
}
#endif

static int qlic_native_tile_bands_enabled(void) {
#ifdef QLIC_STREAM_TRACE
  const char *enable = getenv("QLIC_ENABLE_NATIVE_TILE_BANDS");
  const char *disable = getenv("QLIC_DISABLE_NATIVE_TILE_BANDS");
  return enable && enable[0] && enable[0] != '0' &&
         !(disable && disable[0] && disable[0] != '0');
#else
  return 0;
#endif
}

static int qlic_native_rgba_tile_bands_enabled(void) {
#ifdef QLIC_STREAM_TRACE
  const char *disable = getenv("QLIC_DISABLE_NATIVE_RGBA_TILE_BANDS");
  return !(disable && disable[0] && disable[0] != '0');
#else
  return 1;
#endif
}

/* Benchmark builds retain the rejected broad/oracle routes for reproducible
   evidence.  The narrow RGBA route above is production behavior, with a
   trace-build kill switch for matched A/B validation. */
static int qlic_native_tile_bands_oracle_enabled(void) {
#ifdef QLIC_STREAM_TRACE
  const char *value = getenv("QLIC_NATIVE_TILE_BANDS_ORACLE");
  return value && value[0] && value[0] != '0';
#else
  return 0;
#endif
}

#ifdef QLIC_STREAM_TRACE
static int qlic_candidate_trace_enabled(void) {
  const char *value = getenv("QLIC_TRACE_CANDIDATES");
  return value && value[0] && value[0] != '0';
}

static int qlic_cpal_trace_enabled(void) {
  const char *value = getenv("QLIC_TRACE_CPAL");
  return value && value[0] && value[0] != '0';
}

static int qlic_force_deep_trace_enabled(void) {
  const char *value = getenv("QLIC_TRACE_FORCE_DEEP");
  return value && value[0] && value[0] != '0';
}

static double qlic_candidate_trace_now(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
  struct timespec value;
  timespec_get(&value, TIME_UTC);
  return (double)value.tv_sec + (double)value.tv_nsec * 1e-9;
#endif
}

static void qlic_candidate_trace_phase(const char *phase, double begin) {
  if (qlic_candidate_trace_enabled())
    fprintf(stderr, "candidate-phase name=%s seconds=%.9f\n", phase,
            qlic_candidate_trace_now() - begin);
}
#endif

#ifndef QLIC_NO_MAIN
static int rd_img(const wchar_t *path, Image *out);
static int rd_wide_img(const wchar_t *path, uint32_t channels,
                       uint32_t bits_per_sample, uint32_t alpha_mode,
                       WideImage *out);
static double now_s(void);
#endif

const char *qlic_core_error(void) { return g_err; }

QlicCoreStatus qlic_core_status(void) { return g_status; }

void clear_err(void) {
  g_err[0] = 0;
  g_status = QLIC_CORE_OK;
}

void set_err(const char *fmt, ...) {
  if (g_status == QLIC_CORE_OK)
    g_status = QLIC_CORE_ERROR;
#ifdef QLIC_WASM
  size_t n = 0;
  while (fmt[n] && n + 1u < sizeof(g_err)) {
    g_err[n] = fmt[n];
    ++n;
  }
  g_err[n] = 0;
#else
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
#endif
}

void set_err_status(QlicCoreStatus status, const char *fmt, ...) {
  g_status = status;
#ifdef QLIC_WASM
  size_t n = 0;
  while (fmt[n] && n + 1u < sizeof(g_err)) {
    g_err[n] = fmt[n];
    ++n;
  }
  g_err[n] = 0;
#else
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
#endif
}

void qlic_core_default_limits(QlicDecodeLimits *limits) {
  if (limits)
    *limits = g_default_decode_limits;
}

static const QlicDecodeLimits *decode_limits(const QlicDecodeLimits *limits) {
  return limits ? limits : &g_default_decode_limits;
}

static int decode_limits_valid(const QlicDecodeLimits *limits) {
  if (!limits->max_file_bytes || !limits->max_payload_bytes ||
      !limits->max_pixels || !limits->max_animation_bytes ||
      !limits->max_decoded_bytes || !limits->max_metadata_bytes ||
      !limits->max_frames || !limits->max_chunks) {
    set_err("invalid decode limits");
    return 0;
  }
  return 1;
}

#if defined(_WIN32) && !defined(QLIC_NO_MAIN)
static int fail_hr(const char *where, HRESULT hr) {
  set_err_status(hr == E_OUTOFMEMORY ? QLIC_CORE_OUT_OF_MEMORY
                                     : QLIC_CORE_ERROR,
                 "%s failed: 0x%08lx", where, (unsigned long)hr);
  return 0;
}
#endif

#ifdef _WIN32
static int fail_win32(const char *where) {
  DWORD error = GetLastError();
  set_err_status(error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY
                     ? QLIC_CORE_OUT_OF_MEMORY
                     : QLIC_CORE_ERROR,
                 "%s failed: win32=%lu", where, (unsigned long)error);
  return 0;
}
#endif

static QlicCoreStatus stream_failure_status(int error, int decoding) {
  if (error == STREAM_E_ALLOC)
    return QLIC_CORE_OUT_OF_MEMORY;
  return decoding ? QLIC_CORE_BAD_DATA : QLIC_CORE_ERROR;
}

static void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p)
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
  return p;
}

static int mulok(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a) {
    set_err("size overflow");
    return 0;
  }
  *out = a * b;
  return 1;
}

static int addok(size_t a, size_t b, size_t *out) {
  if (b > SIZE_MAX - a) {
    set_err("size overflow");
    return 0;
  }
  *out = a + b;
  return 1;
}

static int buf_reserve(Buf *b, size_t extra) {
  size_t need;
  if (!addok(b->size, extra, &need))
    return 0;
  if (need <= b->cap)
    return 1;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  uint8_t *p = (uint8_t *)realloc(b->data, cap);
  if (!p) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  b->data = p;
  b->cap = cap;
  return 1;
}

static int buf_append(Buf *b, const void *data, size_t n) {
  if (!n)
    return 1;
  if (!data) {
    set_err("internal append error");
    return 0;
  }
  if (!buf_reserve(b, n))
    return 0;
  memcpy(b->data + b->size, data, n);
  b->size += n;
  return 1;
}

static int buf_u8(Buf *b, uint8_t v) { return buf_append(b, &v, 1); }

static int buf_u16le(Buf *b, uint16_t v) {
  uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  return buf_append(b, x, 2);
}

static int buf_u32le(Buf *b, uint32_t v) {
  uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  return buf_append(b, x, 4);
}

static int buf_u64le(Buf *b, uint64_t v) {
  return buf_u32le(b, (uint32_t)v) && buf_u32le(b, (uint32_t)(v >> 32));
}

void buf_free(Buf *b) {
  free(b->data);
  b->data = NULL;
  b->size = 0;
  b->cap = 0;
}

void image_free(Image *im) {
  free(im->rgba);
  im->rgba = NULL;
  im->width = 0;
  im->height = 0;
}

void anim_free(Anim *a) {
  if (a->frames) {
    for (uint32_t i = 0; i < a->count; ++i)
      image_free(&a->frames[i].image);
  }
  free(a->frames);
  memset(a, 0, sizeof(*a));
}

void candidate_free(Candidate *c) {
  free(c->palette);
  free(c->compressed);
  memset(c, 0, sizeof(*c));
}

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
  uint64_t lo = rd32(p);
  uint64_t hi = rd32(p + 4);
  return lo | (hi << 32);
}

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v) {
  wr32(p, (uint32_t)v);
  wr32(p + 4, (uint32_t)(v >> 32));
}

void wide_image_free(WideImage *im) {
  free(im->pixels);
  memset(im, 0, sizeof(*im));
}

void hdr_image_free(HdrImage *im) {
  if (!im)
    return;
  wide_image_free(&im->wide);
  free(im->icc);
  if (im->metadata) {
    for (uint32_t index = 0; index < im->metadata_count; ++index)
      free(im->metadata[index].data);
  }
  free(im->metadata);
  memset(im, 0, sizeof(*im));
}

static unsigned logical_threads(void) {
#ifdef _WIN32
  /* GetSystemInfo misses processors outside the current Windows group */
  DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (n)
    return (unsigned)n;
  SYSTEM_INFO si;
  GetNativeSystemInfo(&si);
  return si.dwNumberOfProcessors ? (unsigned)si.dwNumberOfProcessors : 1u;
#elif defined(QLIC_WASM)
  return 1u;
#else
  long count = sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 && (unsigned long)count <= UINT_MAX ? (unsigned)count : 1u;
#endif
}

unsigned qlic_core_hardware_threads(void) { return logical_threads(); }

unsigned qlic_core_thread_count(void) { return g_threads; }

int qlic_core_set_thread_count(unsigned threads) {
  if (!threads)
    return 0;
  unsigned limit = logical_threads();
  g_threads = threads > limit ? limit : threads;
  stream_set_threads(g_threads);
  return 1;
}

#ifndef QLIC_NO_MAIN
static int eq_arg(const wchar_t *a, const wchar_t *b) {
  if (!a || !b)
    return 0;
  return _wcsicmp(a, b) == 0;
}

static wchar_t *argp(int argc, wchar_t **argv, int i) {
  if (!argv || i < 0 || i >= argc)
    return NULL;
  return argv[i];
}

static const wchar_t *arg_value(int argc, wchar_t **argv,
                                const wchar_t *long_name,
                                const wchar_t *short_name, const wchar_t *def) {
  for (int i = 0; i + 1 < argc; ++i) {
    if (eq_arg(argv[i], L"--"))
      break;
    if (eq_arg(argv[i], long_name) ||
        (short_name && eq_arg(argv[i], short_name))) {
      return argv[i + 1];
    }
  }
  return def;
}

static int opt_takes_value(const wchar_t *s) {
  if (eq_arg(s, L"--save") || eq_arg(s, L"-s") || eq_arg(s, L"--out") ||
      eq_arg(s, L"-o") || eq_arg(s, L"--threads") ||
      eq_arg(s, L"--color-profile") || eq_arg(s, L"--icc") ||
      eq_arg(s, L"--alpha") || eq_arg(s, L"--exif") ||
      eq_arg(s, L"--xmp") || eq_arg(s, L"--iptc") ||
      eq_arg(s, L"--jumbf"))
    return 1;
  return 0;
}

static int pos_args(int argc, wchar_t **argv, int start, wchar_t **out,
                    int max) {
  int n = 0;
  int options = 1;
  for (int i = start; i < argc; ++i) {
    if (options && eq_arg(argv[i], L"--")) {
      options = 0;
      continue;
    }
    if (options && argv[i][0] == L'-') {
      if (opt_takes_value(argv[i]) && i + 1 < argc)
        ++i;
      continue;
    }
    if (n < max)
      out[n] = argv[i];
    ++n;
  }
  return n;
}

static int has_option(int argc, wchar_t **argv, const wchar_t *name) {
  for (int i = 2; i < argc; ++i) {
    if (eq_arg(argv[i], L"--"))
      break;
    if (eq_arg(argv[i], name))
      return 1;
  }
  return 0;
}

void runtime_init(void) { qlic_core_set_thread_count(1u); }

static int thread_count_for(const wchar_t *s, unsigned *out) {
  unsigned n = logical_threads();
  if (!n)
    n = 1u;
  unsigned threads = 0;
  if (!s || !s[0])
    return 0;
  if (eq_arg(s, L"all"))
    threads = n;
  else {
    uint64_t value = 0;
    for (const wchar_t *p = s; *p; ++p) {
      if (*p < L'0' || *p > L'9')
        return 0;
      if (value <= n)
        value = value * 10u + (uint64_t)(*p - L'0');
    }
    if (!value)
      return 0;
    threads = value > n ? n : (unsigned)value;
  }
  *out = threads;
  return 1;
}

/* empirical Windows encoder bounds, not a relation between the algorithms */
enum { CLI_PACK = 1, CLI_UNPACK, CLI_VIEW, CLI_INFO, CLI_VERIFY, CLI_VERSION };

static int cli_kind(const wchar_t *command) {
  if (eq_arg(command, L"pack"))
    return CLI_PACK;
  if (eq_arg(command, L"unpack"))
    return CLI_UNPACK;
  if (eq_arg(command, L"view"))
    return CLI_VIEW;
  if (eq_arg(command, L"info"))
    return CLI_INFO;
  if (eq_arg(command, L"verify"))
    return CLI_VERIFY;
  if (eq_arg(command, L"version") || eq_arg(command, L"--version"))
    return CLI_VERSION;
  return 0;
}

static int cli_validate(int argc, wchar_t **argv, int kind, unsigned *threads,
                        int *has_threads) {
  int positions = 0;
  int save = 0;
  int color_profile = 0;
  int icc = 0;
  int alpha = 0;
  int exif = 0;
  int xmp = 0;
  int iptc = 0;
  int jumbf = 0;
  int options = 1;
  for (int i = 2; i < argc; ++i) {
    const wchar_t *arg = argv[i];
    if (options && eq_arg(arg, L"--")) {
      options = 0;
      continue;
    }
    if (!options || arg[0] != L'-') {
      ++positions;
      continue;
    }
    if (eq_arg(arg, L"--threads")) {
      if (*has_threads || i + 1 >= argc ||
          !thread_count_for(argv[++i], threads)) {
        set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid --threads option");
        return 0;
      }
      if (kind != CLI_PACK && kind != CLI_UNPACK && kind != CLI_VIEW &&
          kind != CLI_VERIFY) {
        set_err_status(QLIC_CORE_BAD_ARGUMENT,
                       "--threads is not valid for this command");
        return 0;
      }
      *has_threads = 1;
      continue;
    }
    if (eq_arg(arg, L"--json")) {
      if (kind != CLI_INFO && kind != CLI_VERIFY) {
        set_err_status(QLIC_CORE_BAD_ARGUMENT,
                       "--json is not valid for this command");
        return 0;
      }
      continue;
    }
    if (eq_arg(arg, L"--color-profile") || eq_arg(arg, L"--icc") ||
        eq_arg(arg, L"--alpha") || eq_arg(arg, L"--exif") ||
        eq_arg(arg, L"--xmp") || eq_arg(arg, L"--iptc") ||
        eq_arg(arg, L"--jumbf")) {
      int *seen = eq_arg(arg, L"--color-profile") ? &color_profile
                  : eq_arg(arg, L"--icc")         ? &icc
                  : eq_arg(arg, L"--alpha")       ? &alpha
                  : eq_arg(arg, L"--exif")        ? &exif
                  : eq_arg(arg, L"--xmp")         ? &xmp
                  : eq_arg(arg, L"--iptc")        ? &iptc
                                                  : &jumbf;
      if (kind != CLI_PACK || *seen || i + 1 >= argc || !argv[i + 1][0]) {
        set_err_status(QLIC_CORE_BAD_ARGUMENT,
                       "invalid self-describing image option");
        return 0;
      }
      *seen = 1;
      ++i;
      continue;
    }
    if (eq_arg(arg, L"--save") || eq_arg(arg, L"-s") || eq_arg(arg, L"--out") ||
        eq_arg(arg, L"-o")) {
      if (kind != CLI_VIEW || save || i + 1 >= argc) {
        set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid view output option");
        return 0;
      }
      save = 1;
      ++i;
      continue;
    }
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "unknown command option");
    return 0;
  }
  int expected = kind == CLI_PACK || kind == CLI_UNPACK ? 2
                 : kind == CLI_VIEW || kind == CLI_INFO || kind == CLI_VERIFY
                     ? 1
                     : 0;
  if (positions != expected) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "wrong number of command arguments");
    return 0;
  }
  return 1;
}

static uint64_t fsize(const wchar_t *path) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data))
    return 0;
  return ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

static int rd_file(const wchar_t *path, Buf *out) {
  FILE *f = NULL;
  if (_wfopen_s(&f, path, L"rb") != 0 || !f) {
    set_err("could not open input file");
    return 0;
  }
  if (_fseeki64(f, 0, SEEK_END) != 0) {
    fclose(f);
    set_err("could not seek input file");
    return 0;
  }
  __int64 n = _ftelli64(f);
  if (n < 0) {
    fclose(f);
    set_err("could not measure input file");
    return 0;
  }
  if ((uint64_t)n > SIZE_MAX ||
      (uint64_t)n > g_default_decode_limits.max_file_bytes) {
    fclose(f);
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: file bytes");
    return 0;
  }
  if (_fseeki64(f, 0, SEEK_SET) != 0) {
    fclose(f);
    set_err("could not rewind input file");
    return 0;
  }
  out->data = (uint8_t *)xmalloc((size_t)n);
  if (!out->data) {
    fclose(f);
    return 0;
  }
  out->size = (size_t)n;
  out->cap = (size_t)n;
  if (out->size && fread(out->data, 1, out->size, f) != out->size) {
    buf_free(out);
    fclose(f);
    set_err("could not read input file");
    return 0;
  }
  fclose(f);
  return 1;
}

static int wr_file(const wchar_t *path, const uint8_t *data, size_t n) {
  FILE *f = NULL;
  if (_wfopen_s(&f, path, L"wb") != 0 || !f) {
    set_err("could not open output file");
    return 0;
  }
  if (n && fwrite(data, 1, n, f) != n) {
    fclose(f);
    set_err("could not write output file");
    return 0;
  }
  fclose(f);
  return 1;
}

static int tmp_ext(const wchar_t *ext, wchar_t *out, size_t cap) {
  wchar_t dir[MAX_PATH];
  wchar_t tmp[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, dir);
  if (n == 0 || n >= MAX_PATH)
    return 0;
  if (!GetTempFileNameW(dir, L"qli", 0, tmp))
    return 0;
  DeleteFileW(tmp);
  if (wcslen(tmp) + wcslen(ext) + 1 > cap)
    return 0;
  wcscpy_s(out, cap, tmp);
  wcscat_s(out, cap, ext);
  return 1;
}
#endif

#if defined(_WIN32) && !defined(QLIC_NO_MAIN)
static int rd_frame(IWICImagingFactory *fac, IWICBitmapFrameDecode *frame,
                    Image *out, uint32_t expected_width,
                    uint32_t expected_height,
                    const QlicDecodeLimits *decode_limit,
                    uint64_t max_output_bytes) {
  HRESULT hr;
  IWICFormatConverter *conv = NULL;
  UINT w = 0, h = 0;

  hr = frame->lpVtbl->GetSize(frame, &w, &h);
  if (FAILED(hr))
    return fail_hr("WIC size", hr);
  if (!w || !h) {
    set_err("invalid image dimensions");
    return 0;
  }
  if ((expected_width && w != expected_width) ||
      (expected_height && h != expected_height)) {
    set_err("corrupt file: embedded image dimensions mismatch");
    return 0;
  }
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  uint64_t pixels = (uint64_t)w * h;
  if (pixels > limits->max_pixels || pixels > UINT64_MAX / 4u) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED, "resource limit exceeded: pixels");
    return 0;
  }
  uint64_t stride64 = (uint64_t)w * 4u;
  uint64_t bytes64 = pixels * 4u;
  if (bytes64 > max_output_bytes || stride64 > UINT_MAX || bytes64 > UINT_MAX ||
      bytes64 > SIZE_MAX) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: WIC pixel buffer");
    return 0;
  }

  hr = fac->lpVtbl->CreateFormatConverter(fac, &conv);
  if (FAILED(hr)) {
    return fail_hr("WIC converter", hr);
  }

  hr = conv->lpVtbl->Initialize(
      conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppRGBA,
      WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    conv->lpVtbl->Release(conv);
    return fail_hr("WIC convert to RGBA", hr);
  }

  size_t bytes = (size_t)bytes64;

  out->rgba = (uint8_t *)xmalloc(bytes);
  if (!out->rgba) {
    conv->lpVtbl->Release(conv);
    return 0;
  }
  out->width = (uint32_t)w;
  out->height = (uint32_t)h;

  hr = conv->lpVtbl->CopyPixels(conv, NULL, (UINT)stride64, (UINT)bytes64,
                                out->rgba);
  conv->lpVtbl->Release(conv);
  if (FAILED(hr)) {
    image_free(out);
    return fail_hr("WIC pixels", hr);
  }
  return 1;
}

static uint32_t rd_frame_delay(IWICBitmapFrameDecode *frame) {
  IWICMetadataQueryReader *qr = NULL;
  PROPVARIANT v;
  PropVariantInit(&v);
  uint32_t ms = 100;
  HRESULT hr = frame->lpVtbl->GetMetadataQueryReader(frame, &qr);
  if (SUCCEEDED(hr)) {
    hr = qr->lpVtbl->GetMetadataByName(qr, L"/grctlext/Delay", &v);
    if (SUCCEEDED(hr)) {
      uint32_t ticks = 0;
      if (v.vt == VT_UI1)
        ticks = v.bVal;
      else if (v.vt == VT_UI2)
        ticks = v.uiVal;
      else if (v.vt == VT_UI4)
        ticks = v.ulVal;
      if (ticks)
        ms = ticks * 10u;
    }
    PropVariantClear(&v);
    qr->lpVtbl->Release(qr);
  }
  return ms;
}

static int rd_anim(const wchar_t *path, Anim *out, int *is_anim) {
  if (is_anim)
    *is_anim = 0;
  HRESULT hr;
  IWICImagingFactory *fac = NULL;
  IWICBitmapDecoder *dec = NULL;
  UINT count = 0;
  QlicDecodeLimits limits = g_default_decode_limits;

  hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        &IID_IWICImagingFactory, (void **)&fac);
  if (FAILED(hr))
    return fail_hr("WIC factory", hr);

  hr = fac->lpVtbl->CreateDecoderFromFilename(
      fac, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
  if (FAILED(hr)) {
    fac->lpVtbl->Release(fac);
    return fail_hr("WIC decoder", hr);
  }

  hr = dec->lpVtbl->GetFrameCount(dec, &count);
  if (FAILED(hr)) {
    dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
    return fail_hr("WIC frame count", hr);
  }
  if (count <= 1) {
    dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
    return 1;
  }
  if (count > limits.max_frames) {
    dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: animation frames");
    return 0;
  }

  out->frames = (AnimFrame *)calloc(count, sizeof(*out->frames));
  if (!out->frames) {
    dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  out->count = count;
  out->loop_count = 0;

  int ok = 1;
  uint64_t animation_bytes = (uint64_t)count * sizeof(*out->frames);
  if (animation_bytes > limits.max_animation_bytes) {
    dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
    anim_free(out);
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: animation memory");
    return 0;
  }
  for (UINT i = 0; i < count; ++i) {
    IWICBitmapFrameDecode *frame = NULL;
    hr = dec->lpVtbl->GetFrame(dec, i, &frame);
    if (FAILED(hr)) {
      fail_hr("WIC animation frame", hr);
      ok = 0;
      break;
    }
    ok = rd_frame(fac, frame, &out->frames[i].image, i ? out->width : 0u,
                  i ? out->height : 0u, &limits,
                  limits.max_animation_bytes - animation_bytes);
    out->frames[i].delay_ms = rd_frame_delay(frame);
    frame->lpVtbl->Release(frame);
    if (!ok)
      break;
    uint64_t frame_bytes =
        (uint64_t)out->frames[i].image.width * out->frames[i].image.height * 4u;
    animation_bytes += frame_bytes;
    if (i == 0) {
      out->width = out->frames[i].image.width;
      out->height = out->frames[i].image.height;
    }
  }
  dec->lpVtbl->Release(dec);
  fac->lpVtbl->Release(fac);
  if (!ok) {
    anim_free(out);
    return 0;
  }
  if (is_anim)
    *is_anim = 1;
  return 1;
}

static int rd_img(const wchar_t *path, Image *out) {
  HRESULT hr;
  IWICImagingFactory *fac = NULL;
  IWICBitmapDecoder *dec = NULL;
  IWICBitmapFrameDecode *frame = NULL;

  hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        &IID_IWICImagingFactory, (void **)&fac);
  if (FAILED(hr))
    return fail_hr("WIC factory", hr);

  hr = fac->lpVtbl->CreateDecoderFromFilename(
      fac, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
  if (FAILED(hr)) {
    fac->lpVtbl->Release(fac);
    return fail_hr("WIC decoder", hr);
  }

  hr = dec->lpVtbl->GetFrame(dec, 0, &frame);
  if (FAILED(hr)) {
    dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
    return fail_hr("WIC first frame", hr);
  }

  int ok = rd_frame(fac, frame, out, 0, 0, &g_default_decode_limits,
                    g_default_decode_limits.max_animation_bytes);
  frame->lpVtbl->Release(frame);
  dec->lpVtbl->Release(dec);
  fac->lpVtbl->Release(fac);
  return ok;
}

static int rd_wic_icc(const wchar_t *path, Buf *profile) {
  memset(profile, 0, sizeof(*profile));
  HRESULT hr;
  IWICImagingFactory *factory = NULL;
  IWICBitmapDecoder *decoder = NULL;
  IWICBitmapFrameDecode *frame = NULL;
  IWICColorContext **contexts = NULL;
  UINT count = 0;
  UINT actual = 0;
  hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        &IID_IWICImagingFactory, (void **)&factory);
  if (SUCCEEDED(hr))
    hr = factory->lpVtbl->CreateDecoderFromFilename(
        factory, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
        &decoder);
  if (SUCCEEDED(hr))
    hr = decoder->lpVtbl->GetFrame(decoder, 0u, &frame);
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->GetColorContexts(frame, 0u, NULL, &count);
  if (FAILED(hr) || !count) {
    if (frame)
      frame->lpVtbl->Release(frame);
    if (decoder)
      decoder->lpVtbl->Release(decoder);
    if (factory)
      factory->lpVtbl->Release(factory);
    return 1;
  }
  if (count > 16u) {
    frame->lpVtbl->Release(frame);
    decoder->lpVtbl->Release(decoder);
    factory->lpVtbl->Release(factory);
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "source contains too many color contexts");
    return 0;
  }
  contexts = (IWICColorContext **)calloc(count, sizeof(*contexts));
  if (!contexts) {
    frame->lpVtbl->Release(frame);
    decoder->lpVtbl->Release(decoder);
    factory->lpVtbl->Release(factory);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  for (UINT index = 0; index < count && SUCCEEDED(hr); ++index)
    hr = factory->lpVtbl->CreateColorContext(factory, contexts + index);
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->GetColorContexts(frame, count, contexts, &actual);
  int ok = SUCCEEDED(hr);
  if (ok && actual > count) {
    set_err_status(QLIC_CORE_ERROR,
                   "WIC returned an inconsistent color context count");
    ok = 0;
  }
  for (UINT index = 0; ok && index < actual; ++index) {
    WICColorContextType type = WICColorContextUninitialized;
    UINT bytes = 0;
    if (FAILED(contexts[index]->lpVtbl->GetType(contexts[index], &type)) ||
        type != WICColorContextProfile ||
        FAILED(contexts[index]->lpVtbl->GetProfileBytes(contexts[index], 0u,
                                                        NULL, &bytes)) ||
        !bytes)
      continue;
    if (bytes > QLIC_DEFAULT_MAX_METADATA_BYTES) {
      set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                     "embedded ICC profile exceeds the metadata limit");
      ok = 0;
      break;
    }
    profile->data = (uint8_t *)xmalloc(bytes);
    if (!profile->data) {
      ok = 0;
      break;
    }
    UINT written = 0;
    if (FAILED(contexts[index]->lpVtbl->GetProfileBytes(
            contexts[index], bytes, profile->data, &written)) ||
        written != bytes) {
      buf_free(profile);
      continue;
    }
    profile->size = bytes;
    profile->cap = bytes;
    break;
  }
  for (UINT index = 0; index < count; ++index)
    if (contexts[index])
      contexts[index]->lpVtbl->Release(contexts[index]);
  free(contexts);
  frame->lpVtbl->Release(frame);
  decoder->lpVtbl->Release(decoder);
  factory->lpVtbl->Release(factory);
  return ok;
}

static int rd_wide_img(const wchar_t *path, uint32_t channels,
                       uint32_t bits_per_sample, uint32_t alpha_mode,
                       WideImage *out) {
  if (!out || bits_per_sample != 16u ||
      (channels != 1u && channels != 3u && channels != 4u)) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "this asset decoder requires 16-bit gray, RGB, or RGBA");
    return 0;
  }
  memset(out, 0, sizeof(*out));
  HRESULT hr;
  IWICImagingFactory *fac = NULL;
  IWICBitmapDecoder *dec = NULL;
  IWICBitmapFrameDecode *frame = NULL;
  IWICFormatConverter *conv = NULL;
  UINT w = 0, h = 0;
  const WICPixelFormatGUID *format =
      channels == 1u
          ? &GUID_WICPixelFormat16bppGray
          : channels == 3u
                ? &GUID_WICPixelFormat48bppRGB
                : alpha_mode == 2u
                      ? &GUID_WICPixelFormat64bppPRGBA
                      : &GUID_WICPixelFormat64bppRGBA;

  hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        &IID_IWICImagingFactory, (void **)&fac);
  if (FAILED(hr))
    return fail_hr("WIC factory", hr);
  hr = fac->lpVtbl->CreateDecoderFromFilename(
      fac, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
  if (SUCCEEDED(hr))
    hr = dec->lpVtbl->GetFrame(dec, 0, &frame);
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->GetSize(frame, &w, &h);
  if (FAILED(hr)) {
    if (frame)
      frame->lpVtbl->Release(frame);
    if (dec)
      dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
    return fail_hr("WIC 16-bit decoder", hr);
  }
  uint64_t pixels = (uint64_t)w * h;
  uint64_t stride64 = (uint64_t)w * channels * sizeof(uint16_t);
  uint64_t bytes64 = stride64 * h;
  if (!w || !h || pixels > g_default_decode_limits.max_pixels ||
      stride64 > UINT_MAX || bytes64 > UINT_MAX || bytes64 > SIZE_MAX ||
      bytes64 > g_default_decode_limits.max_decoded_bytes) {
    frame->lpVtbl->Release(frame);
    dec->lpVtbl->Release(dec);
    fac->lpVtbl->Release(fac);
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: 16-bit pixel buffer");
    return 0;
  }
  hr = fac->lpVtbl->CreateFormatConverter(fac, &conv);
  if (SUCCEEDED(hr))
    hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame, format,
                                  WICBitmapDitherTypeNone, NULL, 0.0,
                                  WICBitmapPaletteTypeCustom);
  if (SUCCEEDED(hr)) {
    out->pixels = xmalloc((size_t)bytes64);
    if (!out->pixels)
      hr = E_OUTOFMEMORY;
  }
  if (SUCCEEDED(hr))
    hr = conv->lpVtbl->CopyPixels(conv, NULL, (UINT)stride64, (UINT)bytes64,
                                  (BYTE *)out->pixels);
  if (conv)
    conv->lpVtbl->Release(conv);
  frame->lpVtbl->Release(frame);
  dec->lpVtbl->Release(dec);
  fac->lpVtbl->Release(fac);
  if (FAILED(hr)) {
    wide_image_free(out);
    return fail_hr("WIC 16-bit pixels", hr);
  }
  out->width = w;
  out->height = h;
  out->channels = channels;
  out->bits_per_sample = bits_per_sample;
  out->pixels_size = (size_t)bytes64;
  out->stride = (size_t)stride64;
  return 1;
}
#endif

#ifndef QLIC_NO_MAIN
static int has_ext(const wchar_t *path, const wchar_t *ext) {
  const wchar_t *dot = wcsrchr(path, L'.');
  return dot && _wcsicmp(dot, ext) == 0;
}

static const GUID *ct_for(const wchar_t *path) {
  if (has_ext(path, L".bmp"))
    return &GUID_ContainerFormatBmp;
  if (has_ext(path, L".tif") || has_ext(path, L".tiff"))
    return &GUID_ContainerFormatTiff;
  return &GUID_ContainerFormatPng;
}

static void try_wic_profile(IWICImagingFactory *factory,
                            IWICBitmapFrameEncode *frame,
                            const uint8_t *icc, size_t icc_size) {
  if (!factory || !frame || !icc || !icc_size || icc_size > UINT_MAX)
    return;
  IWICColorContext *context = NULL;
  HRESULT hr = factory->lpVtbl->CreateColorContext(factory, &context);
  if (SUCCEEDED(hr))
    hr = context->lpVtbl->InitializeFromMemory(context, icc, (UINT)icc_size);
  if (SUCCEEDED(hr))
    (void)frame->lpVtbl->SetColorContexts(frame, 1u, &context);
  if (context)
    context->lpVtbl->Release(context);
}

static int wr_ppm(const wchar_t *path, const Image *im) {
  size_t pixels, bytes;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels) ||
      !mulok(pixels, 3, &bytes))
    return 0;
  uint8_t *rgb = (uint8_t *)xmalloc(bytes);
  if (!rgb)
    return 0;
  for (size_t i = 0, j = 0; i < pixels; ++i, j += 3) {
    const uint8_t *p = im->rgba + i * 4;
    if (p[3] != 255) {
      free(rgb);
      set_err("PPM cannot preserve alpha; use PNG or TIFF");
      return 0;
    }
    rgb[j + 0] = p[0];
    rgb[j + 1] = p[1];
    rgb[j + 2] = p[2];
  }

  FILE *f = NULL;
  if (_wfopen_s(&f, path, L"wb") != 0 || !f) {
    free(rgb);
    set_err("could not open output file");
    return 0;
  }
  fprintf(f, "P6\n%u %u\n255\n", im->width, im->height);
  int ok = fwrite(rgb, 1, bytes, f) == bytes;
  fclose(f);
  free(rgb);
  if (!ok) {
    set_err("could not write output file");
    return 0;
  }
  return 1;
}

static int wr_img_profile_alpha(const wchar_t *path, const Image *im,
                                const uint8_t *icc, size_t icc_size,
                                int premultiplied) {
  if (has_ext(path, L".ppm"))
    return wr_ppm(path, im);
  if (premultiplied &&
      !(has_ext(path, L".tif") || has_ext(path, L".tiff"))) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "premultiplied alpha requires associated-alpha TIFF");
    return 0;
  }

  HRESULT hr;
  IWICImagingFactory *fac = NULL;
  IWICStream *stream = NULL;
  IWICBitmapEncoder *enc = NULL;
  IWICBitmapFrameEncode *frame = NULL;
  IPropertyBag2 *bag = NULL;

  size_t pixels, bytes;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels) ||
      !mulok(pixels, 4, &bytes))
    return 0;
  uint8_t *bgra = NULL;
  const uint8_t *wic_pixels = im->rgba;
  if (!premultiplied) {
    bgra = (uint8_t *)xmalloc(bytes);
    if (!bgra)
      return 0;
    for (size_t i = 0; i < pixels; ++i) {
      const uint8_t *s = im->rgba + i * 4;
      uint8_t *d = bgra + i * 4;
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
      d[3] = s[3];
    }
    wic_pixels = bgra;
  }

  hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        &IID_IWICImagingFactory, (void **)&fac);
  if (FAILED(hr)) {
    free(bgra);
    return fail_hr("WIC factory", hr);
  }
  hr = fac->lpVtbl->CreateStream(fac, &stream);
  if (FAILED(hr)) {
    fac->lpVtbl->Release(fac);
    free(bgra);
    return fail_hr("WIC stream", hr);
  }
  hr = stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE);
  if (FAILED(hr)) {
    stream->lpVtbl->Release(stream);
    fac->lpVtbl->Release(fac);
    free(bgra);
    return fail_hr("WIC output file", hr);
  }
  hr = fac->lpVtbl->CreateEncoder(fac, ct_for(path), NULL, &enc);
  if (FAILED(hr)) {
    stream->lpVtbl->Release(stream);
    fac->lpVtbl->Release(fac);
    free(bgra);
    return fail_hr("WIC encoder", hr);
  }
  hr = enc->lpVtbl->Initialize(enc, (IStream *)stream, WICBitmapEncoderNoCache);
  if (FAILED(hr)) {
    enc->lpVtbl->Release(enc);
    stream->lpVtbl->Release(stream);
    fac->lpVtbl->Release(fac);
    free(bgra);
    return fail_hr("WIC encoder init", hr);
  }
  hr = enc->lpVtbl->CreateNewFrame(enc, &frame, &bag);
  if (FAILED(hr)) {
    enc->lpVtbl->Release(enc);
    stream->lpVtbl->Release(stream);
    fac->lpVtbl->Release(fac);
    free(bgra);
    return fail_hr("WIC frame", hr);
  }
  hr = frame->lpVtbl->Initialize(frame, bag);
  if (SUCCEEDED(hr))
    try_wic_profile(fac, frame, icc, icc_size);
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->SetSize(frame, im->width, im->height);
  WICPixelFormatGUID pf = premultiplied ? GUID_WICPixelFormat32bppPRGBA
                                        : GUID_WICPixelFormat32bppBGRA;
  WICPixelFormatGUID wanted = pf;
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->SetPixelFormat(frame, &pf);
  if (SUCCEEDED(hr) && !IsEqualGUID(&pf, &wanted)) {
    hr = E_FAIL;
  }
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->WritePixels(frame, im->height, im->width * 4,
                                    (UINT)bytes, (BYTE *)wic_pixels);
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->Commit(frame);
  if (SUCCEEDED(hr))
    hr = enc->lpVtbl->Commit(enc);

  if (bag)
    bag->lpVtbl->Release(bag);
  if (frame)
    frame->lpVtbl->Release(frame);
  if (enc)
    enc->lpVtbl->Release(enc);
  if (stream)
    stream->lpVtbl->Release(stream);
  if (fac)
    fac->lpVtbl->Release(fac);
  free(bgra);
  if (FAILED(hr))
    return fail_hr("WIC write", hr);
  return 1;
}

static int wr_img_profile(const wchar_t *path, const Image *im,
                          const uint8_t *icc, size_t icc_size) {
  return wr_img_profile_alpha(path, im, icc, icc_size, 0);
}

static int wr_img(const wchar_t *path, const Image *im) {
  return wr_img_profile(path, im, NULL, 0u);
}

static const char *mname(int mode) {
  static const char *const names[] = {
      "unknown",    "gray",      "gray+alpha", "rgb",
      "rgba",       "palette",   "source",     "separable",
      NULL,         "native",    "filtered",   "palette-filtered",
      "ppal",       "cpalette",  "tiles",      "tile-model",
      "gray-model", "animation", "blocks",     "native-wide",
      "hdr-wide"};
  return mode >= 0 && (size_t)mode < sizeof(names) / sizeof(names[0]) &&
                 names[mode]
             ? names[mode]
             : "unknown";
}

static const char *tname(int transform) {
  static const char *const names[] = {
      "identity",        "green-delta",     "identity-raw",
      "green-delta-raw", "identity-rle",    "green-delta-rle",
      "index-rle",       "separable-delta", "red-delta",
      "blue-delta",      "cpalette-delta",  "blue-delta-planar-med",
      "cpalette-tiles",  "cpalette-planar"};
  return transform >= 0 && (size_t)transform < sizeof(names) / sizeof(names[0])
             ? names[transform]
             : "unknown";
}
#endif

static int is_raw(int transform) {
  return transform == TRANSFORM_IDENTITY_RAW ||
         transform == TRANSFORM_GDELTA_RAW;
}

static int is_gd(int transform) {
  return transform == TRANSFORM_GDELTA || transform == TRANSFORM_GDELTA_RAW ||
         transform == TRANSFORM_GDELTA_RLE;
}

static int is_rd(int transform) { return transform == TRANSFORM_RDELTA; }

static int is_bd(int transform) { return transform == TRANSFORM_BDELTA; }

static int is_planar_med(int transform) {
  return transform == TRANSFORM_BDELTA_PLANAR_MED;
}

static int is_cpal_tiles(int transform) {
  return transform == TRANSFORM_CPAL_TILES;
}

static int is_cpal_planar(int transform) {
  return transform == TRANSFORM_CPAL_PLANAR;
}

static int is_rle(int transform) {
  return transform == TRANSFORM_IDENTITY_RLE ||
         transform == TRANSFORM_GDELTA_RLE;
}

#ifndef QLIC_NO_MAIN
static const char *cname(int codec) {
  static const char *const names[] = {"store", "xpress", "xpress-huff", "lzms"};
  return codec >= 0 && (size_t)codec < sizeof(names) / sizeof(names[0])
             ? names[codec]
             : "unknown";
}
#endif

static int mbpp(int mode) {
  return mode >= MODE_GRAY && mode <= MODE_RGBA ? mode : 0;
}

static int scan_mode(const Image *im) {
  size_t pixels;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels))
    return MODE_RGBA;
  int opaque = 1;
  int gray = 1;
  for (size_t i = 0; i < pixels && (opaque || gray); ++i) {
    const uint8_t *p = im->rgba + i * 4;
    if (p[3] != 255)
      opaque = 0;
    if (p[0] != p[1] || p[0] != p[2])
      gray = 0;
  }
  if (gray && opaque)
    return MODE_GRAY;
  if (gray)
    return MODE_GRAYA;
  if (opaque)
    return MODE_RGB;
  return MODE_RGBA;
}

static int mk_samp(const Image *im, int mode, int transform, Buf *out) {
  size_t pixels;
  int bpp = mbpp(mode);
  if (bpp <= 0) {
    set_err("invalid sample mode");
    return 0;
  }
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels))
    return 0;
  size_t bytes;
  if (!mulok(pixels, (size_t)bpp, &bytes))
    return 0;
  if (!buf_reserve(out, bytes))
    return 0;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t *p = im->rgba + i * 4;
    uint8_t r = p[0], g = p[1], b = p[2], a = p[3];
    if (mode == MODE_GRAY) {
      out->data[out->size++] = r;
    } else if (mode == MODE_GRAYA) {
      out->data[out->size++] = r;
      out->data[out->size++] = a;
    } else if (mode == MODE_RGB) {
      if (is_gd(transform)) {
        out->data[out->size++] = g;
        out->data[out->size++] = (uint8_t)(r - g);
        out->data[out->size++] = (uint8_t)(b - g);
      } else if (is_rd(transform)) {
        out->data[out->size++] = r;
        out->data[out->size++] = (uint8_t)(g - r);
        out->data[out->size++] = (uint8_t)(b - r);
      } else if (is_bd(transform)) {
        out->data[out->size++] = b;
        out->data[out->size++] = (uint8_t)(r - b);
        out->data[out->size++] = (uint8_t)(g - b);
      } else {
        out->data[out->size++] = r;
        out->data[out->size++] = g;
        out->data[out->size++] = b;
      }
    } else if (mode == MODE_RGBA) {
      if (is_gd(transform)) {
        out->data[out->size++] = g;
        out->data[out->size++] = (uint8_t)(r - g);
        out->data[out->size++] = (uint8_t)(b - g);
        out->data[out->size++] = a;
      } else if (is_rd(transform)) {
        out->data[out->size++] = r;
        out->data[out->size++] = (uint8_t)(g - r);
        out->data[out->size++] = (uint8_t)(b - r);
        out->data[out->size++] = a;
      } else if (is_bd(transform)) {
        out->data[out->size++] = b;
        out->data[out->size++] = (uint8_t)(r - b);
        out->data[out->size++] = (uint8_t)(g - b);
        out->data[out->size++] = a;
      } else {
        out->data[out->size++] = r;
        out->data[out->size++] = g;
        out->data[out->size++] = b;
        out->data[out->size++] = a;
      }
    }
  }
  return 1;
}

static int mk_sep(const Image *im, int base_mode, Buf *out) {
  int bpp = mbpp(base_mode);
  if (bpp <= 0 || !im->width || !im->height)
    return 0;
  size_t row_stride;
  if (!mulok((size_t)im->width, 4u, &row_stride))
    return -1;
  int channel[4] = {0, 1, 2, 3};
  if (base_mode == MODE_GRAYA)
    channel[1] = 3;
  const uint8_t *origin = im->rgba;
  for (uint32_t y = 1; y < im->height; ++y) {
    const uint8_t *column = im->rgba + (size_t)y * row_stride;
    for (uint32_t x = 1; x < im->width; ++x) {
      const uint8_t *row = im->rgba + (size_t)x * 4u;
      const uint8_t *pixel = column + (size_t)x * 4u;
      for (int ch = 0; ch < bpp; ++ch) {
        int c = channel[ch];
        if ((uint8_t)(row[c] + column[c] - origin[c]) != pixel[c])
          return 0;
      }
    }
  }
  size_t entries;
  if (!addok((size_t)im->width, (size_t)im->height - 1u, &entries) ||
      !mulok(entries, (size_t)bpp, &entries) || !buf_reserve(out, entries))
    return -1;
  for (uint32_t x = 0; x < im->width; ++x) {
    const uint8_t *pixel = im->rgba + (size_t)x * 4u;
    for (int ch = 0; ch < bpp; ++ch)
      out->data[out->size++] = pixel[channel[ch]];
  }
  for (uint32_t y = 1; y < im->height; ++y) {
    const uint8_t *pixel = im->rgba + (size_t)y * row_stride;
    for (int ch = 0; ch < bpp; ++ch)
      out->data[out->size++] = pixel[channel[ch]];
  }
  return 1;
}

#ifndef QLIC_NO_MAIN
static int wr_wide_img_profile_alpha(const wchar_t *path, const WideImage *im,
                                     const uint8_t *icc, size_t icc_size,
                                     int premultiplied) {
  if (!im || !im->pixels || im->bits_per_sample != 16u ||
      (im->channels != 1u && im->channels != 3u && im->channels != 4u)) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "asset output currently requires 16-bit samples");
    return 0;
  }
  if (!has_ext(path, L".png") && !has_ext(path, L".tif") &&
      !has_ext(path, L".tiff")) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "16-bit output requires PNG or TIFF");
    return 0;
  }
  if (premultiplied &&
      !(has_ext(path, L".tif") || has_ext(path, L".tiff"))) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "premultiplied alpha requires associated-alpha TIFF");
    return 0;
  }
  size_t row_bytes = 0, bytes = 0, prefix = 0, required = 0;
  if (!mulok((size_t)im->width, (size_t)im->channels * sizeof(uint16_t),
             &row_bytes) ||
      !mulok(row_bytes, (size_t)im->height, &bytes) || !im->width ||
      !im->height || im->stride < row_bytes ||
      !mulok((size_t)(im->height - 1u), im->stride, &prefix) ||
      !addok(prefix, row_bytes, &required) || im->pixels_size < required ||
      row_bytes > UINT_MAX || bytes > UINT_MAX) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid 16-bit image layout");
    return 0;
  }

  HRESULT hr;
  IWICImagingFactory *fac = NULL;
  IWICStream *stream = NULL;
  IWICBitmapEncoder *enc = NULL;
  IWICBitmapFrameEncode *frame = NULL;
  IPropertyBag2 *bag = NULL;
  const WICPixelFormatGUID *wanted =
      im->channels == 1u
          ? &GUID_WICPixelFormat16bppGray
          : im->channels == 3u
                ? &GUID_WICPixelFormat48bppRGB
                : premultiplied ? &GUID_WICPixelFormat64bppPRGBA
                                : &GUID_WICPixelFormat64bppRGBA;

  hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                        &IID_IWICImagingFactory, (void **)&fac);
  if (SUCCEEDED(hr))
    hr = fac->lpVtbl->CreateStream(fac, &stream);
  if (SUCCEEDED(hr))
    hr = stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE);
  if (SUCCEEDED(hr))
    hr = fac->lpVtbl->CreateEncoder(fac, ct_for(path), NULL, &enc);
  if (SUCCEEDED(hr))
    hr = enc->lpVtbl->Initialize(enc, (IStream *)stream,
                                 WICBitmapEncoderNoCache);
  if (SUCCEEDED(hr))
    hr = enc->lpVtbl->CreateNewFrame(enc, &frame, &bag);
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->Initialize(frame, bag);
  if (SUCCEEDED(hr))
    try_wic_profile(fac, frame, icc, icc_size);
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->SetSize(frame, im->width, im->height);
  WICPixelFormatGUID actual = *wanted;
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->SetPixelFormat(frame, &actual);
  if (SUCCEEDED(hr) && !IsEqualGUID(&actual, wanted))
    hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
  if (SUCCEEDED(hr)) {
    if (im->stride == row_bytes) {
      hr = frame->lpVtbl->WritePixels(frame, im->height, (UINT)row_bytes,
                                      (UINT)bytes, (BYTE *)im->pixels);
    } else {
      for (uint32_t y = 0; y < im->height && SUCCEEDED(hr); ++y) {
        BYTE *row = (BYTE *)im->pixels + (size_t)y * im->stride;
        hr = frame->lpVtbl->WritePixels(frame, 1u, (UINT)row_bytes,
                                        (UINT)row_bytes, row);
      }
    }
  }
  if (SUCCEEDED(hr))
    hr = frame->lpVtbl->Commit(frame);
  if (SUCCEEDED(hr))
    hr = enc->lpVtbl->Commit(enc);
  if (bag)
    bag->lpVtbl->Release(bag);
  if (frame)
    frame->lpVtbl->Release(frame);
  if (enc)
    enc->lpVtbl->Release(enc);
  if (stream)
    stream->lpVtbl->Release(stream);
  if (fac)
    fac->lpVtbl->Release(fac);
  if (FAILED(hr))
    return fail_hr("WIC 16-bit write", hr);
  return 1;
}

static int wr_wide_img_profile(const wchar_t *path, const WideImage *im,
                               const uint8_t *icc, size_t icc_size) {
  return wr_wide_img_profile_alpha(path, im, icc, icc_size, 0);
}

static int wr_wide_img(const wchar_t *path, const WideImage *im) {
  return wr_wide_img_profile(path, im, NULL, 0u);
}
#endif

static int mk_sepd(const Buf *sep, uint32_t width, uint32_t height, int bpp,
                   Buf *out) {
  size_t sbpp = (size_t)bpp;
  size_t row_bytes;
  if (!mulok((size_t)width, sbpp, &row_bytes))
    return 0;
  size_t col_bytes, expected;
  if (!mulok((size_t)(height - 1u), sbpp, &col_bytes))
    return 0;
  if (!addok(row_bytes, col_bytes, &expected))
    return 0;
  if (sep->size != expected) {
    set_err("internal separable table size mismatch");
    return 0;
  }
  const uint8_t *row0 = sep->data;
  const uint8_t *cols = sep->data + row_bytes;
  if (!buf_append(out, row0, sbpp))
    return 0;
  for (uint32_t x = 1; x < width; ++x) {
    for (int ch = 0; ch < bpp; ++ch) {
      size_t c = (size_t)ch;
      if (!buf_u8(out, (uint8_t)(row0[(size_t)x * sbpp + c] -
                                 row0[(size_t)(x - 1u) * sbpp + c])))
        return 0;
    }
  }
  const uint8_t *prev = row0;
  for (uint32_t y = 1; y < height; ++y) {
    const uint8_t *cur = cols + (size_t)(y - 1u) * sbpp;
    for (int ch = 0; ch < bpp; ++ch) {
      size_t c = (size_t)ch;
      if (!buf_u8(out, (uint8_t)(cur[c] - prev[c])))
        return 0;
    }
    prev = cur;
  }
  return 1;
}

static int pae(int a, int b, int c) {
  int p = a + b - c;
  int pa = abs(p - a);
  int pb = abs(p - b);
  int pc = abs(p - c);
  if (pa <= pb && pa <= pc)
    return a;
  if (pb <= pc)
    return b;
  return c;
}

static int pred(int filter, int left, int up, int up_left) {
  switch (filter) {
  case 0:
    return 0;
  case 1:
    return left;
  case 2:
    return up;
  case 3:
    return (left + up) >> 1;
  case 4:
    return pae(left, up, up_left);
  case 5: {
    int g = left + up - up_left;
    if (g < 0)
      return 0;
    if (g > 255)
      return 255;
    return g;
  }
  default:
    return 0;
  }
}

static uint64_t fscore(const uint8_t *row, const uint8_t *prev,
                       size_t row_bytes, int bpp, int filter) {
  size_t sbpp = (size_t)bpp;
  uint64_t score = 0;
  for (size_t x = 0; x < row_bytes; ++x) {
    int left = x >= sbpp ? row[x - sbpp] : 0;
    int up = prev ? prev[x] : 0;
    int up_left = (prev && x >= sbpp) ? prev[x - sbpp] : 0;
    int pr = pred(filter, left, up, up_left);
    uint8_t r = (uint8_t)(row[x] - pr);
    score += r < 128 ? r : 256 - r;
  }
  return score;
}

static int pick_f(const uint8_t *row, const uint8_t *prev, size_t row_bytes,
                  int bpp, int y, int search) {
  if (search == 0)
    return y == 0 ? 1 : 5;
  static const int basic_filters[] = {1, 2, 5};
  static const int extended_filters[] = {1, 2, 3, 4, 5};
  const int *filters = search >= 2 ? extended_filters : basic_filters;
  int filter_count = search >= 2 ? 5 : 3;
  uint64_t best_score = UINT64_MAX;
  int best = 0;
  for (int i = 0; i < filter_count; ++i) {
    int f = filters[i];
    uint64_t s = fscore(row, prev, row_bytes, bpp, f);
    if (s < best_score) {
      best_score = s;
      best = f;
    }
  }
  return best;
}

static int mk_frows(const uint8_t *samples, size_t row_bytes, uint32_t height,
                    int bpp, int search, Buf *out) {
  size_t sbpp = (size_t)bpp;
  size_t total_row, total;
  if (!addok(row_bytes, 1, &total_row))
    return 0;
  if (!mulok(total_row, (size_t)height, &total))
    return 0;
  if (!buf_reserve(out, total))
    return 0;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *row = samples + (size_t)y * row_bytes;
    const uint8_t *prev = y ? row - row_bytes : NULL;
    int filter = pick_f(row, prev, row_bytes, bpp, (int)y, search);
    out->data[out->size++] = (uint8_t)filter;
    for (size_t x = 0; x < row_bytes; ++x) {
      int left = x >= sbpp ? row[x - sbpp] : 0;
      int up = prev ? prev[x] : 0;
      int up_left = (prev && x >= sbpp) ? prev[x - sbpp] : 0;
      int pr = pred(filter, left, up, up_left);
      out->data[out->size++] = (uint8_t)(row[x] - pr);
    }
  }
  return 1;
}

static int unf_rows(const uint8_t *payload, size_t payload_size,
                    size_t row_bytes, uint32_t height, int bpp, Buf *samples) {
  size_t sbpp = (size_t)bpp;
  size_t expected_row, expected;
  if (!addok(row_bytes, 1, &expected_row))
    return 0;
  if (!mulok(expected_row, (size_t)height, &expected))
    return 0;
  if (expected != payload_size) {
    set_err("corrupt file: unexpected payload size");
    return 0;
  }
  size_t sample_size;
  if (!mulok(row_bytes, (size_t)height, &sample_size))
    return 0;
  if (!buf_reserve(samples, sample_size))
    return 0;
  samples->size = sample_size;
  size_t in = 0;
  for (uint32_t y = 0; y < height; ++y) {
    int filter = payload[in++];
    if (filter < 0 || filter > 5) {
      set_err("corrupt file: bad filter");
      return 0;
    }
    uint8_t *row = samples->data + (size_t)y * row_bytes;
    uint8_t *prev = y ? row - row_bytes : NULL;
    for (size_t x = 0; x < row_bytes; ++x) {
      int left = x >= sbpp ? row[x - sbpp] : 0;
      int up = prev ? prev[x] : 0;
      int up_left = (prev && x >= sbpp) ? prev[x - sbpp] : 0;
      int pr = pred(filter, left, up, up_left);
      row[x] = (uint8_t)(payload[in++] + pr);
    }
  }
  return 1;
}

static size_t np2(size_t x) {
  size_t p = 1;
  while (p < x && p <= SIZE_MAX / 2)
    p <<= 1;
  return p;
}

static uint32_t ckey(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint32_t hkey(uint32_t k) {
  k ^= k >> 16;
  k *= 0x7feb352du;
  k ^= k >> 15;
  k *= 0x846ca68bu;
  k ^= k >> 16;
  return k;
}

static int map_put(MapEntry *map, size_t mask, uint32_t key, uint32_t *value,
                   int add, uint32_t new_value) {
  size_t pos = (size_t)hkey(key) & mask;
  for (;;) {
    if (!map[pos].used) {
      if (!add)
        return 0;
      map[pos].used = 1;
      map[pos].key = key;
      map[pos].value = new_value;
      *value = new_value;
      return 1;
    }
    if (map[pos].key == key) {
      *value = map[pos].value;
      return 1;
    }
    pos = (pos + 1) & mask;
  }
}

static int pal_bits(uint32_t count) {
  if (count <= 2u)
    return 1;
  if (count <= 4u)
    return 2;
  if (count <= 16u)
    return 4;
  if (count <= 256u)
    return 8;
  int bits = 1;
  uint32_t maxv = count > 0 ? count - 1u : 0;
  while (bits < 16 && (maxv >> bits))
    ++bits;
  return bits;
}

static int valid_index_bits(int bits) { return bits >= 1 && bits <= 16; }

static size_t row_pack(uint32_t width, int bits) {
  return ((size_t)width * (size_t)bits + 7u) >> 3;
}

static int pack_row(const uint16_t *ids, uint32_t width, int bits, Buf *out) {
  if (bits == 8) {
    for (uint32_t x = 0; x < width; ++x) {
      if (!buf_u8(out, (uint8_t)ids[x]))
        return 0;
    }
    return 1;
  }
  if (bits == 16) {
    for (uint32_t x = 0; x < width; ++x) {
      if (!buf_u16le(out, ids[x]))
        return 0;
    }
    return 1;
  }

  uint32_t acc = 0;
  int used = 0;
  uint32_t mask = (1u << bits) - 1u;
  for (uint32_t x = 0; x < width; ++x) {
    acc |= ((uint32_t)ids[x] & mask) << used;
    used += bits;
    while (used >= 8) {
      if (!buf_u8(out, (uint8_t)(acc & 255u)))
        return 0;
      acc >>= 8;
      used -= 8;
    }
  }
  if (used > 0 && !buf_u8(out, (uint8_t)(acc & 255u)))
    return 0;
  return 1;
}

static uint32_t unpack_i(const uint8_t *row, uint32_t x, int bits) {
  if (bits == 8)
    return row[x];
  if (bits == 16)
    return rd16(row + (size_t)x * 2u);
  size_t bit = (size_t)x * (size_t)bits;
  size_t byte = bit >> 3;
  int shift = (int)(bit & 7u);
  int need = (shift + bits + 7) >> 3;
  uint32_t v = 0;
  for (int i = 0; i < need; ++i)
    v |= (uint32_t)row[byte + (size_t)i] << (i * 8);
  return (v >> shift) & ((1u << bits) - 1u);
}

static int buf_varint(Buf *b, size_t v) {
  while (v >= 128u) {
    if (!buf_u8(b, (uint8_t)((v & 127u) | 128u)))
      return 0;
    v >>= 7;
  }
  return buf_u8(b, (uint8_t)v);
}

static int read_varint(const uint8_t *data, size_t size, size_t *pos,
                       size_t *v) {
  size_t out = 0;
  int shift = 0;
  int bits = (int)(sizeof(size_t) * 8);
  while (*pos < size) {
    uint8_t c = data[(*pos)++];
    size_t chunk = (size_t)(c & 127u);
    if (shift >= bits || (chunk && chunk > (SIZE_MAX >> shift))) {
      set_err("corrupt file: bad run length");
      return 0;
    }
    out |= chunk << shift;
    if (!(c & 128u)) {
      *v = out;
      return 1;
    }
    shift += 7;
  }
  set_err("corrupt file: bad run length");
  return 0;
}

static int rle_encode(const uint8_t *data, size_t size, Buf *out) {
  size_t i = 0;
  while (i < size) {
    uint8_t v = data[i];
    size_t run = 1;
    while (i + run < size && data[i + run] == v)
      ++run;
    if (!buf_varint(out, run - 1) || !buf_u8(out, v))
      return 0;
    i += run;
  }
  return 1;
}

static int rle_decode(const uint8_t *data, size_t size, size_t expected,
                      Buf *out) {
  if (!buf_reserve(out, expected))
    return 0;
  out->size = 0;
  size_t pos = 0;
  while (pos < size) {
    size_t runm1 = 0;
    if (!read_varint(data, size, &pos, &runm1))
      return 0;
    if (pos >= size) {
      set_err("corrupt file: truncated run");
      return 0;
    }
    uint8_t v = data[pos++];
    if (runm1 == SIZE_MAX) {
      set_err("corrupt file: run length overflow");
      return 0;
    }
    size_t run = runm1 + 1u;
    if (run > expected - out->size) {
      set_err("corrupt file: run exceeds expected size");
      return 0;
    }
    memset(out->data + out->size, v, run);
    out->size += run;
  }
  if (out->size != expected) {
    set_err("corrupt file: run payload size mismatch");
    return 0;
  }
  return 1;
}

static int enc_irun(const uint16_t *ids, size_t pixels, Buf *runs) {
  size_t i = 0;
  while (i < pixels) {
    uint16_t v = ids[i];
    size_t run = 1;
    while (i + run < pixels && ids[i + run] == v)
      ++run;
    if (!buf_varint(runs, run - 1) || !buf_varint(runs, v))
      return 0;
    i += run;
  }
  return 1;
}

static int enc_ppal(const uint16_t *ids, uint32_t width, uint32_t height,
                    Buf *out) {
  size_t pixels;
  if (!mulok((size_t)width, (size_t)height, &pixels))
    return 0;
  size_t pos = 0;
  while (pos < pixels) {
    uint32_t x = (uint32_t)(pos % width);
    uint32_t y = (uint32_t)(pos / width);
    uint16_t v = ids[pos];
    if (x > 0 && v == ids[pos - 1]) {
      size_t run = 1;
      while (pos + run < pixels) {
        size_t p = pos + run;
        uint32_t xx = (uint32_t)(p % width);
        if (xx == 0 || ids[p] != ids[p - 1])
          break;
        ++run;
      }
      if (!buf_u8(out, 0) || !buf_varint(out, run - 1))
        return 0;
      pos += run;
    } else if (y > 0 && v == ids[pos - width]) {
      size_t run = 1;
      while (pos + run < pixels) {
        size_t p = pos + run;
        if (p < width || ids[p] != ids[p - width])
          break;
        ++run;
      }
      if (!buf_u8(out, 1) || !buf_varint(out, run - 1))
        return 0;
      pos += run;
    } else if (x > 0 && y > 0 && v == ids[pos - width - 1]) {
      size_t run = 1;
      while (pos + run < pixels) {
        size_t p = pos + run;
        uint32_t xx = (uint32_t)(p % width);
        if (xx == 0 || p < width + 1u || ids[p] != ids[p - width - 1])
          break;
        ++run;
      }
      if (!buf_u8(out, 2) || !buf_varint(out, run - 1))
        return 0;
      pos += run;
    } else {
      if (!buf_u8(out, 3) || !buf_varint(out, v))
        return 0;
      ++pos;
    }
  }
  return 1;
}

typedef struct {
  uint32_t old_index;
  uint8_t c[4];
} PalSort;

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
typedef struct {
  uint32_t old_index;
  uint32_t run_count;
  uint8_t hot;
  uint8_t c[4];
} PalRunSort;
#endif

static int pal_sort_cmp(const void *ap, const void *bp) {
  const PalSort *a = (const PalSort *)ap;
  const PalSort *b = (const PalSort *)bp;
  if (a->c[0] != b->c[0])
    return a->c[0] < b->c[0] ? -1 : 1;
  if (a->c[1] != b->c[1])
    return a->c[1] < b->c[1] ? -1 : 1;
  if (a->c[2] != b->c[2])
    return a->c[2] < b->c[2] ? -1 : 1;
  if (a->c[3] != b->c[3])
    return a->c[3] < b->c[3] ? -1 : 1;
  return 0;
}

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
static int pal_run_sort_cmp(const void *ap, const void *bp) {
  const PalRunSort *a = (const PalRunSort *)ap;
  const PalRunSort *b = (const PalRunSort *)bp;
  if (a->run_count != b->run_count)
    return a->run_count > b->run_count ? -1 : 1;
  if (a->c[0] != b->c[0])
    return a->c[0] < b->c[0] ? -1 : 1;
  if (a->c[1] != b->c[1])
    return a->c[1] < b->c[1] ? -1 : 1;
  if (a->c[2] != b->c[2])
    return a->c[2] < b->c[2] ? -1 : 1;
  if (a->c[3] != b->c[3])
    return a->c[3] < b->c[3] ? -1 : 1;
  return 0;
}

static int pal_hot_lex_sort_cmp(const void *ap, const void *bp) {
  const PalRunSort *a = (const PalRunSort *)ap;
  const PalRunSort *b = (const PalRunSort *)bp;
  if (a->hot != b->hot)
    return a->hot ? -1 : 1;
  if (a->c[0] != b->c[0])
    return a->c[0] < b->c[0] ? -1 : 1;
  if (a->c[1] != b->c[1])
    return a->c[1] < b->c[1] ? -1 : 1;
  if (a->c[2] != b->c[2])
    return a->c[2] < b->c[2] ? -1 : 1;
  if (a->c[3] != b->c[3])
    return a->c[3] < b->c[3] ? -1 : 1;
  return 0;
}

/* CPAL run IDs below 128 take one varint byte instead of two. Put the 128
   colors that begin the most runs in that range, then retain lexical order
   inside both partitions so palette deltas remain compressible. */
static int sort_palette_map_by_hot_runs(Buf *palette, uint16_t *map,
                                        const uint32_t *run_counts,
                                        uint32_t count) {
  size_t pal_bytes;
  if (!mulok((size_t)count, 4u, &pal_bytes))
    return 0;
  PalRunSort *ord = (PalRunSort *)xmalloc((size_t)count * sizeof(*ord));
  uint8_t *pal = (uint8_t *)xmalloc(pal_bytes);
  if (!ord || !pal) {
    free(ord);
    free(pal);
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {
    ord[i].old_index = i;
    ord[i].run_count = run_counts[i];
    ord[i].hot = 0;
    memcpy(ord[i].c, palette->data + (size_t)i * 4u, 4u);
  }
  qsort(ord, count, sizeof(*ord), pal_run_sort_cmp);
  for (uint32_t i = 0; i < count && i < 128u; ++i)
    ord[i].hot = 1;
  qsort(ord, count, sizeof(*ord), pal_hot_lex_sort_cmp);
  for (uint32_t i = 0; i < count; ++i) {
    memcpy(pal + (size_t)i * 4u, ord[i].c, 4u);
    map[ord[i].old_index] = (uint16_t)i;
  }
  memcpy(palette->data, pal, pal_bytes);
  free(ord);
  free(pal);
  return 1;
}
#endif

#ifdef QLIC_WASM
static void pal_sort_wasm(PalSort *items, uint32_t count) {
  for (uint32_t start = count / 2u; start > 0;) {
    --start;
    uint32_t root = start;
    for (;;) {
      uint32_t child = root * 2u + 1u;
      if (child >= count)
        break;
      if (child + 1u < count &&
          pal_sort_cmp(&items[child], &items[child + 1u]) < 0)
        ++child;
      if (pal_sort_cmp(&items[root], &items[child]) >= 0)
        break;
      PalSort swap = items[root];
      items[root] = items[child];
      items[child] = swap;
      root = child;
    }
  }
  for (uint32_t end = count; end > 1u;) {
    --end;
    PalSort swap = items[0];
    items[0] = items[end];
    items[end] = swap;
    uint32_t root = 0;
    for (;;) {
      uint32_t child = root * 2u + 1u;
      if (child >= end)
        break;
      if (child + 1u < end &&
          pal_sort_cmp(&items[child], &items[child + 1u]) < 0)
        ++child;
      if (pal_sort_cmp(&items[root], &items[child]) >= 0)
        break;
      swap = items[root];
      items[root] = items[child];
      items[child] = swap;
      root = child;
    }
  }
}
#endif

static int sort_palette_map(Buf *palette, uint16_t *map, uint32_t count) {
  size_t pal_bytes;
  if (!mulok((size_t)count, 4u, &pal_bytes))
    return 0;
  PalSort *ord = (PalSort *)xmalloc((size_t)count * sizeof(PalSort));
  uint8_t *pal = (uint8_t *)xmalloc(pal_bytes);
  if (!ord || !pal) {
    free(ord);
    free(pal);
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *c = palette->data + (size_t)i * 4u;
    ord[i].old_index = i;
    memcpy(ord[i].c, c, 4);
  }
#ifdef QLIC_WASM
  pal_sort_wasm(ord, count);
#else
  qsort(ord, count, sizeof(PalSort), pal_sort_cmp);
#endif
  for (uint32_t i = 0; i < count; ++i) {
    memcpy(pal + (size_t)i * 4u, ord[i].c, 4);
    map[ord[i].old_index] = (uint16_t)i;
  }
  memcpy(palette->data, pal, pal_bytes);
  free(ord);
  free(pal);
  return 1;
}

static int sort_palette(Buf *palette, uint16_t *ids, size_t pixels,
                        uint32_t count) {
  if (count <= 1)
    return 1;
  uint16_t *map = (uint16_t *)xmalloc((size_t)count * sizeof(uint16_t));
  if (!map)
    return 0;
  if (!sort_palette_map(palette, map, count)) {
    free(map);
    return 0;
  }
  for (size_t i = 0; i < pixels; ++i)
    ids[i] = map[ids[i]];
  free(map);
  return 1;
}

static int probe_palette_count(const Image *im, uint32_t limit,
                               uint32_t *unique_count) {
  size_t pixels;
  if (unique_count)
    *unique_count = limit + 1u;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels))
    return 0;
  size_t cap = np2((size_t)limit * 4u);
  MapEntry *map = (MapEntry *)calloc(cap, sizeof(MapEntry));
  if (!map)
    return 1;
  uint32_t count = 0;
  int ok = 1;
  for (size_t i = 0; i < pixels; ++i) {
    uint32_t value = 0;
    if (!map_put(map, cap - 1, ckey(im->rgba + i * 4u), &value, 0, 0)) {
      if (count >= limit) {
        ok = 0;
        break;
      }
      value = count++;
      if (!map_put(map, cap - 1, ckey(im->rgba + i * 4u), &value, 1, value))
        break;
    }
  }
  free(map);
  if (unique_count && ok)
    *unique_count = count;
  return ok;
}

static int probe_palette(const Image *im, uint32_t limit) {
  return probe_palette_count(im, limit, NULL);
}

enum {
  DEEP_BLOCK_PALETTE_PROBE_MAX_PIXELS = 2000000,
  DEEP_BLOCK_PALETTE_PIXELS_PER_COLOR = 32,
  DEEP_BLOCK_PALETTE_MIN_COLORS = 256
};

static int has_sparse_large_palette(const Image *im, size_t pixels) {
  if (pixels > DEEP_BLOCK_PALETTE_PROBE_MAX_PIXELS)
    return 0;
  uint32_t palette_limit =
      (uint32_t)(pixels / DEEP_BLOCK_PALETTE_PIXELS_PER_COLOR);
  if (palette_limit <= DEEP_BLOCK_PALETTE_MIN_COLORS)
    return 0;
  uint32_t exact_palette_count = palette_limit + 1u;
  return probe_palette_count(im, palette_limit, &exact_palette_count) &&
         exact_palette_count <= palette_limit;
}

static int mk_pal(const Image *im, uint32_t limit, Buf *palette, Buf *indices,
                  Buf *index_runs, Buf *pred_runs, int *index_bits,
                  uint32_t *palette_count, uint16_t **raw_ids_out) {
  if (raw_ids_out)
    *raw_ids_out = NULL;
  size_t pixels;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels))
    return -1;
  if (limit < 2)
    return 0;
  size_t cap = np2((size_t)limit * 4u);
  MapEntry *map = (MapEntry *)calloc(cap, sizeof(MapEntry));
  if (!map) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return -1;
  }
  uint32_t count = 0;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t *p = im->rgba + i * 4;
    uint32_t key = ckey(p);
    uint32_t value = 0;
    if (!map_put(map, cap - 1, key, &value, 0, 0)) {
      if (count >= limit) {
        free(map);
        return 0;
      }
      value = count++;
      if (!map_put(map, cap - 1, key, &value, 1, value)) {
        free(map);
        set_err("internal palette map error");
        return -1;
      }
      if (!buf_append(palette, p, 4)) {
        free(map);
        return -1;
      }
    }
  }
  if (count > 65536u) {
    free(map);
    return 0;
  }
  uint16_t *ids = (uint16_t *)xmalloc(pixels * sizeof(uint16_t));
  if (!ids) {
    free(map);
    return -1;
  }
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t *p = im->rgba + i * 4;
    uint32_t value = 0;
    if (!map_put(map, cap - 1, ckey(p), &value, 0, 0)) {
      free(ids);
      free(map);
      set_err("internal palette map error");
      return -1;
    }
    ids[i] = (uint16_t)value;
  }
  if (!sort_palette(palette, ids, pixels, count)) {
    free(ids);
    free(map);
    return -1;
  }
  *index_bits = pal_bits(count);
  *palette_count = count;

  if (indices) {
    size_t row_bytes = row_pack(im->width, *index_bits);
    size_t index_bytes;
    if (!mulok(row_bytes, (size_t)im->height, &index_bytes) ||
        !buf_reserve(indices, index_bytes)) {
      free(ids);
      free(map);
      return -1;
    }
    for (uint32_t y = 0; y < im->height; ++y) {
      if (!pack_row(ids + (size_t)y * im->width, im->width, *index_bits,
                    indices)) {
        free(ids);
        free(map);
        return -1;
      }
    }
  }
  if (index_runs && !enc_irun(ids, pixels, index_runs)) {
    free(ids);
    free(map);
    return -1;
  }
  if (pred_runs && !enc_ppal(ids, im->width, im->height, pred_runs)) {
    free(ids);
    free(map);
    return -1;
  }
  if (raw_ids_out)
    *raw_ids_out = ids;
  else
    free(ids);
  free(map);
  return 1;
}

#ifdef _WIN32
static DWORD alg(int codec) {
  switch (codec) {
  case CODEC_XPRESS:
    return COMPRESS_ALGORITHM_XPRESS;
  case CODEC_XPRESS_HUFF:
    return COMPRESS_ALGORITHM_XPRESS_HUFF;
  case CODEC_LZMS:
    return COMPRESS_ALGORITHM_LZMS;
  default:
    return 0;
  }
}
#endif

#ifdef QLIC_HAVE_WIMLIB
static int zip_wimlib(const uint8_t *src, size_t src_size, int codec,
                      Buf *out) {
  static const uint8_t signature[8] = {0x0a, 0x51, 0xe5, 0xc0,
                                       0x18, 0x00, 0x00, 0x00};
  /* Windows uses 64 KiB blocks here, matching it avoids another format path */
  const size_t block_size = 65536u;
  enum wimlib_compression_type type = codec == CODEC_LZMS
                                          ? WIMLIB_COMPRESSION_TYPE_LZMS
                                          : WIMLIB_COMPRESSION_TYPE_XPRESS;
  unsigned level = codec == CODEC_LZMS ? QLIC_WIMLIB_LZMS_LEVEL : 20u;
  size_t blocks = src_size / block_size + (src_size % block_size != 0u);
  size_t overhead;
  size_t cap;
  if (!mulok(blocks, 4u, &overhead) ||
      !addok(overhead, sizeof(signature) + 16u, &overhead) ||
      !addok(src_size, overhead, &cap)) {
    set_err("compression buffer is too large");
    return 0;
  }
  uint8_t *data = (uint8_t *)xmalloc(cap);
  if (!data)
    return 0;
  struct wimlib_compressor *compressor = NULL;
  if (wimlib_create_compressor(type, block_size, level, &compressor) != 0) {
    free(data);
    set_err("could not create the Linux compression backend");
    return 0;
  }
  memcpy(data, signature, sizeof(signature));
  wr64(data + 8, src_size);
  wr64(data + 16, block_size);
  size_t input = 0;
  size_t output = 24u;
  while (input < src_size) {
    size_t count = src_size - input;
    if (count > block_size)
      count = block_size;
    size_t compressed =
        wimlib_compress(src + input, count, data + output + 4u,
                        count > 0u ? count - 1u : 0u, compressor);
    if (!compressed) {
      compressed = count;
      memcpy(data + output + 4u, src + input, count);
    }
    wr32(data + output, (uint32_t)compressed);
    output += 4u + compressed;
    input += count;
  }
  wimlib_free_compressor(compressor);
  out->data = data;
  out->size = output;
  out->cap = cap;
  return 1;
}
#endif

static int zip(const uint8_t *src, size_t src_size, int codec, Buf *out) {
  if (codec == CODEC_STORE)
    return buf_append(out, src, src_size);

#ifdef _WIN32
  COMPRESSOR_HANDLE h = NULL;
  if (!CreateCompressor(alg(codec), NULL, &h))
    return fail_win32("CreateCompressor");
  size_t cap = 0;
  if (!addok(src_size, src_size / 8, &cap) || !addok(cap, 4096u, &cap)) {
    CloseCompressor(h);
    return 0;
  }
  if (cap < 256)
    cap = 256;
  out->data = (uint8_t *)calloc(cap, 1u);
  if (!out->data) {
    CloseCompressor(h);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  out->cap = cap;
  out->size = 0;
  for (int tries = 0; tries < 12; ++tries) {
    SIZE_T got = 0;
    if (Compress(h, src, src_size, out->data, out->cap, &got)) {
      out->size = got;
      CloseCompressor(h);
      return 1;
    }
    DWORD err = GetLastError();
    if (err != ERROR_INSUFFICIENT_BUFFER) {
      CloseCompressor(h);
      int ok = fail_win32("Compress");
      buf_free(out);
      return ok;
    }
    if (out->cap > SIZE_MAX / 2 && got <= out->cap) {
      CloseCompressor(h);
      buf_free(out);
      set_err("compression buffer is too large");
      return 0;
    }
    size_t next = got > out->cap ? got : out->cap * 2;
    uint8_t *p = (uint8_t *)realloc(out->data, next);
    if (!p) {
      CloseCompressor(h);
      buf_free(out);
      set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
      return 0;
    }
    out->data = p;
    out->cap = next;
    memset(out->data, 0, out->cap);
  }
  CloseCompressor(h);
  buf_free(out);
  set_err("compression buffer growth exhausted");
  return 0;
#elif defined(QLIC_HAVE_WIMLIB)
  if (codec == CODEC_LZMS || codec == CODEC_XPRESS_HUFF)
    return zip_wimlib(src, src_size, codec, out);
  set_err("unsupported outer compression method");
  return 0;
#else
  set_err("outer compression is unavailable on this platform");
  return 0;
#endif
}

#ifdef QLIC_STREAM_TRACE
static int trace_cpal_zip(const char *variant, const uint8_t *data, size_t size,
                          uint32_t palette_count, int index_bits,
                          uint32_t width, uint32_t height) {
  Buf compressed = {0};
  double begin = qlic_candidate_trace_now();
  int ok = zip(data, size, CODEC_LZMS, &compressed);
  double seconds = qlic_candidate_trace_now() - begin;
  fprintf(stderr,
          "cpal-probe variant=%s colors=%u bits=%d width=%u height=%u "
          "payload=%zu bytes=%zu seconds=%.9f error=%d\n",
          variant, palette_count, index_bits, width, height, size,
          ok ? compressed.size : 0u, seconds, ok ? 0 : 1);
  buf_free(&compressed);
  return ok;
}

static void trace_cpal_make_payload(uint8_t *dst, const Buf *palette,
                                    const Buf *indices, size_t row_bytes,
                                    uint32_t height, int predictor_bpp,
                                    int palette_delta, int filter) {
  for (size_t i = 0; i < palette->size; ++i)
    dst[i] = palette_delta && i >= 4u
                 ? (uint8_t)(palette->data[i] - palette->data[i - 4u])
                 : palette->data[i];
  uint8_t *out = dst + palette->size;
  size_t bpp = (size_t)predictor_bpp;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *row = indices->data + (size_t)y * row_bytes;
    const uint8_t *prev = y ? row - row_bytes : NULL;
    for (size_t x = 0; x < row_bytes; ++x) {
      int left = x >= bpp ? row[x - bpp] : 0;
      int up = prev ? prev[x] : 0;
      int up_left = prev && x >= bpp ? prev[x - bpp] : 0;
      out[(size_t)y * row_bytes + x] =
          (uint8_t)(row[x] - pred(filter, left, up, up_left));
    }
  }
}

static uint8_t trace_cpal_index_predictor(const uint8_t *row,
                                          const uint8_t *prev, size_t x,
                                          int predictor) {
  if (!prev)
    return x ? row[x - 1u] : 0;
  if (!x)
    return prev[0];
  uint8_t left = row[x - 1u];
  uint8_t up = prev[x];
  uint8_t up_left = prev[x - 1u];
  switch (predictor) {
  case 1:
    return left;
  case 2:
    return up;
  case 3:
    return left == up_left ? up : left;
  case 4:
    return up == up_left ? left : up;
  default:
    return (uint8_t)pred(4, left, up, up_left);
  }
}

static void trace_cpal_make_index_map(uint8_t *dst, const Buf *palette,
                                      const Buf *indices, size_t row_bytes,
                                      uint32_t height, int palette_delta,
                                      int predictor, int stable) {
  for (size_t i = 0; i < palette->size; ++i)
    dst[i] = palette_delta && i >= 4u
                 ? (uint8_t)(palette->data[i] - palette->data[i - 4u])
                 : palette->data[i];
  uint8_t *out = dst + palette->size;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *row = indices->data + (size_t)y * row_bytes;
    const uint8_t *prev = y ? row - row_bytes : NULL;
    for (size_t x = 0; x < row_bytes; ++x) {
      uint8_t actual = row[x];
      uint8_t predicted = trace_cpal_index_predictor(row, prev, x, predictor);
      if (actual == predicted)
        out[(size_t)y * row_bytes + x] = 0;
      else if (stable)
        out[(size_t)y * row_bytes + x] =
            actual < predicted ? (uint8_t)(actual + 1u) : actual;
      else
        out[(size_t)y * row_bytes + x] = actual == 0 ? predicted : actual;
    }
  }
}

static int trace_cpal_candidates(const Buf *payload, const Buf *palette,
                                 const Buf *indices, int index_bits,
                                 uint32_t palette_count, uint32_t width,
                                 uint32_t height) {
  size_t row_bytes = row_pack(width, index_bits);
  size_t expected;
  if (!mulok(row_bytes, (size_t)height, &expected) || expected != indices->size)
    return 0;
  uint8_t *work = (uint8_t *)xmalloc(payload->size);
  if (!work)
    return 0;
  int predictor_bpp = index_bits == 16 ? 2 : 1;
  int ok = trace_cpal_zip("raw", payload->data, payload->size, palette_count,
                          index_bits, width, height);
  static const struct {
    const char *name;
    int palette_delta;
    int filter;
  } variants[] = {{"palette-delta", 1, 0},
                  {"left", 0, 1},
                  {"up", 0, 2},
                  {"gradient", 0, 5},
                  {"paeth", 0, 4},
                  {"palette-delta-left", 1, 1},
                  {"palette-delta-up", 1, 2},
                  {"palette-delta-gradient", 1, 5},
                  {"palette-delta-paeth", 1, 4}};
  for (size_t i = 0; ok && i < sizeof(variants) / sizeof(variants[0]); ++i) {
    trace_cpal_make_payload(work, palette, indices, row_bytes, height,
                            predictor_bpp, variants[i].palette_delta,
                            variants[i].filter);
    ok = trace_cpal_zip(variants[i].name, work, payload->size, palette_count,
                        index_bits, width, height);
  }
  static const struct {
    const char *name;
    int palette_delta;
    int predictor;
    int stable;
  } maps[] = {{"swap-left", 0, 1, 0},
              {"swap-up", 0, 2, 0},
              {"swap-edge-left", 0, 3, 0},
              {"swap-edge-up", 0, 4, 0},
              {"swap-paeth", 0, 5, 0},
              {"stable-left", 0, 1, 1},
              {"stable-up", 0, 2, 1},
              {"stable-edge-left", 0, 3, 1},
              {"stable-edge-up", 0, 4, 1},
              {"stable-paeth", 0, 5, 1},
              {"palette-delta-swap-edge-left", 1, 3, 0},
              {"palette-delta-stable-edge-left", 1, 3, 1}};
  if (index_bits == 8) {
    for (size_t i = 0; ok && i < sizeof(maps) / sizeof(maps[0]); ++i) {
      trace_cpal_make_index_map(work, palette, indices, row_bytes, height,
                                maps[i].palette_delta, maps[i].predictor,
                                maps[i].stable);
      ok = trace_cpal_zip(maps[i].name, work, payload->size, palette_count,
                          index_bits, width, height);
    }
  }
  free(work);
  return ok;
}
#endif

static int unzip(const uint8_t *src, size_t src_size, int codec,
                 uint64_t expected64, Buf *out,
                 const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  if (expected64 > limits->max_payload_bytes) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: decoded payload bytes");
    return 0;
  }
  if (expected64 > SIZE_MAX) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: decoded payload bytes");
    return 0;
  }
  size_t expected = (size_t)expected64;
  if (codec == CODEC_STORE && src_size != expected) {
    set_err("corrupt file: stored payload size mismatch");
    return 0;
  }
  if (codec == CODEC_STORE) {
    out->data = (uint8_t *)src;
    out->size = expected;
    return 1;
  }
  out->data = (uint8_t *)xmalloc(expected);
  if (!out->data)
    return 0;
  out->cap = expected;
  out->size = expected;
  if (codec == CODEC_LZMS) {
    if (!qlic_lzms_decompress(src, src_size, out->data, expected)) {
      buf_free(out);
      set_err("corrupt file: invalid LZMS payload");
      return 0;
    }
    return 1;
  }
#ifdef _WIN32
  DECOMPRESSOR_HANDLE h = NULL;
  if (!CreateDecompressor(alg(codec), NULL, &h)) {
    buf_free(out);
    return fail_win32("CreateDecompressor");
  }
  SIZE_T got = 0;
  if (!Decompress(h, src, src_size, out->data, expected, &got)) {
    CloseDecompressor(h);
    buf_free(out);
    return fail_win32("Decompress");
  }
  CloseDecompressor(h);
  if (got != expected) {
    set_err("corrupt file: decompressed size mismatch");
    return 0;
  }
  return 1;
#else
  buf_free(out);
  set_err("this QLIC file uses an unsupported outer codec");
  return 0;
#endif
}

static void unzip_free(Buf *out, int codec) {
  if (codec != CODEC_STORE)
    free(out->data);
  memset(out, 0, sizeof(*out));
}

static size_t cand_size(const Candidate *c) {
  if (!c->compressed)
    return SIZE_MAX;
  size_t size = QLIC_HEADER_SIZE;
  if (!addok(size, c->palette_size, &size) ||
      !addok(size, c->compressed_size, &size))
    return SIZE_MAX;
  return size;
}

static size_t file_palette_size(const QlicHeader *h) {
  return (h->mode == MODE_CPAL || h->mode == MODE_TILES ||
          h->mode == MODE_TILE_MODEL || h->mode == MODE_ANIM ||
          h->mode == MODE_NATIVE_WIDE || h->mode == MODE_HDR_WIDE)
             ? 0u
             : (size_t)h->palette_count * 4u;
}

static int file_palette_mode(int mode) {
  return mode == MODE_PALETTE || mode == MODE_PSTREAM || mode == MODE_PPAL;
}

static int palette_count_ok(uint32_t count, int bits) {
  if (!count || !valid_index_bits(bits))
    return 0;
  if (bits < 16 && count > (1u << bits))
    return 0;
  return count <= 65536u;
}

static void take_best(Candidate *best, Candidate *cand) {
  candidate_free(best);
  *best = *cand;
  memset(cand, 0, sizeof(*cand));
}

static int try_store_owned(Candidate *best, Buf *payload, int mode,
                           int transform, int index_bits,
                           uint32_t palette_count) {
  size_t total = QLIC_HEADER_SIZE;
  if (!addok(total, payload->size, &total)) {
    set_err("size overflow");
    return 0;
  }
  if (total >= cand_size(best))
    return 1;
  Candidate cand;
  memset(&cand, 0, sizeof(cand));
  cand.mode = mode;
  cand.transform = transform;
  cand.index_bits = index_bits;
  cand.codec = CODEC_STORE;
  cand.palette_count = palette_count;
  cand.payload_size = (uint64_t)payload->size;
  cand.compressed = payload->data;
  cand.compressed_size = payload->size;
  payload->data = NULL;
  payload->size = 0;
  payload->cap = 0;
  take_best(best, &cand);
  return 1;
}

enum {
  LZMS_PROXY_DIRECT_BYTES = 4096,
  LZMS_PROXY_REPEAT_DIVISOR = 8,
  LZMS_PROXY_EXTRA_NUMERATOR = 3,
  LZMS_PROXY_EXTRA_DENOMINATOR = 5,
  REPEAT_LZMS_PROXY_EXTRA_NUMERATOR = 16,
  REPEAT_LZMS_PROXY_EXTRA_DENOMINATOR = 100,
  REPEAT_LZMS_PALETTE_MAX_PIXELS = 2000000,
  REPEAT_LZMS_EXACT_PALETTE_MAX_PIXELS = 16000000,
  REPEAT_LZMS_EXACT_PALETTE_MAX_PAYLOAD = 4 * 1024 * 1024
};

#ifndef QLIC_WASM
static int lzms_proxy_worth_trying(size_t payload_size, size_t proxy_size,
                                   size_t proxy_total, size_t incumbent) {
  /* long matches can let LZMS pull much farther ahead than the proxy */
  if (proxy_size <= payload_size / LZMS_PROXY_REPEAT_DIVISOR ||
      proxy_total <= incumbent)
    return 1;
  if (!incumbent || proxy_total / incumbent != 1u)
    return 0;
  size_t excess = proxy_total - incumbent;
  size_t allowance =
      (incumbent / LZMS_PROXY_EXTRA_DENOMINATOR) * LZMS_PROXY_EXTRA_NUMERATOR +
      ((incumbent % LZMS_PROXY_EXTRA_DENOMINATOR) *
       LZMS_PROXY_EXTRA_NUMERATOR) /
          LZMS_PROXY_EXTRA_DENOMINATOR;
  return excess <= allowance;
}
#endif

static int try_pay(Candidate *best, const Buf *payload, const Buf *palette,
                   int mode, int transform, int index_bits,
                   uint32_t palette_count, const int *codecs, int codec_count) {
  for (int i = 0; i < codec_count; ++i) {
    if (codecs[i] == CODEC_STORE) {
      size_t stored_size = QLIC_HEADER_SIZE;
      if (!addok(stored_size, palette ? palette->size : 0u, &stored_size) ||
          !addok(stored_size, payload->size, &stored_size) ||
          stored_size >= cand_size(best))
        continue;
    }
    Candidate cand;
    memset(&cand, 0, sizeof(cand));
    cand.mode = mode;
    cand.transform = transform;
    cand.index_bits = index_bits;
    cand.codec = codecs[i];
    cand.palette_count = palette_count;
    cand.payload_size = (uint64_t)payload->size;
    cand.palette_size = palette ? palette->size : 0;
    Buf comp = {0};
#ifndef QLIC_WASM
    if (codecs[i] == CODEC_LZMS) {
      size_t fixed_size = QLIC_HEADER_SIZE;
      size_t incumbent = cand_size(best);
      int fixed_ok =
          addok(fixed_size, palette ? palette->size : 0u, &fixed_size);
      if (fixed_ok && fixed_size >= incumbent)
        continue;
      if (fixed_ok && payload->size >= LZMS_PROXY_DIRECT_BYTES) {
        Buf proxy = {0};
        int proxy_ok =
            zip(payload->data, payload->size, CODEC_XPRESS_HUFF, &proxy);
        int try_lzms = 1;
        if (proxy_ok) {
          size_t proxy_total = fixed_size;
          if (addok(proxy_total, proxy.size, &proxy_total))
            try_lzms = lzms_proxy_worth_trying(payload->size, proxy.size,
                                               proxy_total, incumbent);
#ifdef QLIC_STREAM_TRACE
          if (qlic_candidate_trace_enabled())
            fprintf(stderr,
                    "candidate-lzms-proxy mode=%d transform=%d payload=%zu "
                    "proxy=%zu total=%zu incumbent=%zu try=%d\n",
                    mode, transform, payload->size, proxy.size, proxy_total,
                    incumbent, try_lzms);
#endif
        } else {
          clear_err();
        }
        buf_free(&proxy);
        if (!try_lzms)
          continue;
      }
    }
#endif
#ifdef QLIC_STREAM_TRACE
    double zip_begin = 0.0;
    int trace_zip = qlic_candidate_trace_enabled();
    if (trace_zip)
      zip_begin = qlic_candidate_trace_now();
#endif
    if (!zip(payload->data, payload->size, codecs[i], &comp)) {
#ifdef QLIC_STREAM_TRACE
      if (trace_zip)
        fprintf(stderr,
                "candidate-zip mode=%d transform=%d codec=%d payload=%zu "
                "bytes=0 incumbent=%zu won=0 seconds=%.9f error=1\n",
                mode, transform, codecs[i], payload->size, cand_size(best),
                qlic_candidate_trace_now() - zip_begin);
#endif
      candidate_free(&cand);
      return 0;
    }
    cand.compressed = comp.data;
    cand.compressed_size = comp.size;
    size_t incumbent = cand_size(best);
    int won = cand_size(&cand) < incumbent;
#ifdef QLIC_STREAM_TRACE
    if (trace_zip)
      fprintf(stderr,
              "candidate-zip mode=%d transform=%d codec=%d payload=%zu "
              "bytes=%zu incumbent=%zu won=%d seconds=%.9f error=0\n",
              mode, transform, codecs[i], payload->size, comp.size, incumbent,
              won, qlic_candidate_trace_now() - zip_begin);
#endif
    if (!won) {
      candidate_free(&cand);
      continue;
    }
    if (cand.palette_size && !cand.palette) {
      cand.palette = (uint8_t *)xmalloc(cand.palette_size);
      if (!cand.palette) {
        candidate_free(&cand);
        return 0;
      }
      memcpy(cand.palette, palette->data, cand.palette_size);
    }
    take_best(best, &cand);
  }
  return 1;
}

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
static int repeat_lzms_proxy(const Buf *payload, const Buf *palette, int mode,
                             int transform, size_t incumbent,
                             size_t *proxy_total_out, int *worth_trying,
                             int ranking_only) {
#ifndef QLIC_STREAM_TRACE
  (void)mode;
  (void)transform;
  (void)ranking_only;
#endif
  *proxy_total_out = SIZE_MAX;
  *worth_trying = 0;
  size_t fixed_size = QLIC_HEADER_SIZE;
  if (!addok(fixed_size, palette ? palette->size : 0u, &fixed_size) ||
      fixed_size >= incumbent)
    return 1;
  Buf proxy = {0};
  if (!zip(payload->data, payload->size, CODEC_XPRESS_HUFF, &proxy)) {
    buf_free(&proxy);
    clear_err();
    return 1;
  }
  size_t proxy_total = fixed_size;
  int proxy_ok = addok(proxy_total, proxy.size, &proxy_total);
  buf_free(&proxy);
  size_t allowance = incumbent / REPEAT_LZMS_PROXY_EXTRA_DENOMINATOR *
                         REPEAT_LZMS_PROXY_EXTRA_NUMERATOR +
                     (incumbent % REPEAT_LZMS_PROXY_EXTRA_DENOMINATOR) *
                         REPEAT_LZMS_PROXY_EXTRA_NUMERATOR /
                         REPEAT_LZMS_PROXY_EXTRA_DENOMINATOR;
  int try_backend = proxy_ok && (proxy_total <= incumbent ||
                                 proxy_total - incumbent <= allowance);
  *proxy_total_out = proxy_ok ? proxy_total : SIZE_MAX;
  *worth_trying = try_backend;
#ifdef QLIC_STREAM_TRACE
  if (!ranking_only && qlic_candidate_trace_enabled())
    fprintf(stderr,
            "candidate-repeat-proxy mode=%d transform=%d payload=%zu "
            "proxy-total=%zu incumbent=%zu allowance=%zu try=%d\n",
            mode, transform, payload->size, proxy_total, incumbent, allowance,
            try_backend);
#endif
  return 1;
}

static int try_pay_repeat_lzms_backend(Candidate *best, const Buf *payload,
                                       const Buf *palette, int mode,
                                       int transform, int index_bits,
                                       uint32_t palette_count,
                                       size_t minimum_saving) {
  size_t incumbent = cand_size(best);
  Buf compressed = {0};
  if (!zip(payload->data, payload->size, CODEC_LZMS, &compressed)) {
    buf_free(&compressed);
    clear_err();
    return 1;
  }
  Candidate candidate;
  memset(&candidate, 0, sizeof(candidate));
  candidate.mode = mode;
  candidate.transform = transform;
  candidate.index_bits = index_bits;
  candidate.codec = CODEC_LZMS;
  candidate.palette_count = palette_count;
  candidate.payload_size = (uint64_t)payload->size;
  candidate.palette_size = palette ? palette->size : 0u;
  candidate.compressed = compressed.data;
  candidate.compressed_size = compressed.size;
#ifdef QLIC_STREAM_TRACE
  if (qlic_candidate_trace_enabled())
    fprintf(stderr,
            "candidate-repeat-lzms mode=%d transform=%d payload=%zu "
            "bytes=%zu incumbent=%zu won=%d\n",
            mode, transform, payload->size, cand_size(&candidate), incumbent,
            cand_size(&candidate) < incumbent);
#endif
  size_t candidate_size = cand_size(&candidate);
  if (candidate_size >= incumbent ||
      incumbent - candidate_size < minimum_saving) {
    candidate_free(&candidate);
    return 1;
  }
  if (candidate.palette_size) {
    candidate.palette = (uint8_t *)xmalloc(candidate.palette_size);
    if (!candidate.palette) {
      candidate_free(&candidate);
      return 0;
    }
    memcpy(candidate.palette, palette->data, candidate.palette_size);
  }
  take_best(best, &candidate);
  return 1;
}

static int try_pay_repeat_lzms(Candidate *best, const Buf *payload,
                               const Buf *palette, int mode, int transform,
                               int index_bits, uint32_t palette_count) {
  size_t proxy_total = SIZE_MAX;
  int worth_trying = 0;
  if (!repeat_lzms_proxy(payload, palette, mode, transform, cand_size(best),
                         &proxy_total, &worth_trying, 0))
    return 0;
  (void)proxy_total;
  return !worth_trying ||
         try_pay_repeat_lzms_backend(best, payload, palette, mode, transform,
                                     index_bits, palette_count, 1u);
}
#endif

static int make_cpal_payload(const Buf *indices, const Buf *palette,
                             int transform, Buf *payload) {
  if (transform == TRANSFORM_CPAL_DELTA) {
    if (!buf_reserve(payload, palette->size))
      return 0;
    for (size_t i = 0; i < palette->size; ++i)
      payload->data[payload->size++] =
          i < 4u ? palette->data[i]
                 : (uint8_t)(palette->data[i] - palette->data[i - 4u]);
  } else if (!buf_append(payload, palette->data, palette->size)) {
    return 0;
  }
  return buf_append(payload, indices->data, indices->size);
}

enum {
  CPAL_PLANAR_INTERLEAVED16,
  CPAL_PLANAR_SPLIT16,
  CPAL_PLANAR_LAYOUT_COUNT
};

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
/* Palette entries are lexicographically sorted by mk_pal.  Channel-planar
   wrapping deltas expose that ordering to the byte compressor.  Indices stay
   fixed-width so decode is one predictable traversal with no run parser. */
static int make_cpal_planar_payload(const Buf *indices, const uint16_t *raw_ids,
                                    const Buf *palette, uint32_t width,
                                    uint32_t height, int index_bits,
                                    uint32_t palette_count, unsigned layout,
                                    Buf *payload) {
  size_t pixels = 0;
  size_t index_bytes = 0;
  size_t total = 0;
  size_t packed_size = 0;
  size_t row_bytes = row_pack(width, index_bits);
  if (layout >= CPAL_PLANAR_LAYOUT_COUNT || palette_count <= 256u ||
      palette_count > 65536u || index_bits != pal_bits(palette_count) ||
      palette->size != (size_t)palette_count * 4u ||
      !mulok((size_t)width, (size_t)height, &pixels) ||
      !mulok(pixels, 2u, &index_bytes) || !addok(1u, palette->size, &total) ||
      !addok(total, index_bytes, &total) || !buf_reserve(payload, total))
    return 0;
  if (!raw_ids &&
      (!indices || !mulok(row_bytes, (size_t)height, &packed_size) ||
       packed_size != indices->size))
    return 0;

  uint8_t *dst = payload->data;
  size_t position = 0;
  dst[position++] = (uint8_t)layout;
  for (unsigned channel = 0; channel < 4u; ++channel) {
    uint8_t previous = 0;
    for (uint32_t i = 0; i < palette_count; ++i) {
      uint8_t value = palette->data[(size_t)i * 4u + channel];
      dst[position++] = i ? (uint8_t)(value - previous) : value;
      previous = value;
    }
  }

  size_t low = position;
  size_t high = layout == CPAL_PLANAR_SPLIT16 ? low + pixels : 0u;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *row = raw_ids ? NULL : indices->data + (size_t)y * row_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      size_t pixel = (size_t)y * width + x;
      uint32_t index = raw_ids ? raw_ids[pixel] : unpack_i(row, x, index_bits);
      if (index >= palette_count) {
        set_err("internal palette index out of range");
        return 0;
      }
      if (layout == CPAL_PLANAR_INTERLEAVED16) {
        dst[position++] = (uint8_t)index;
        dst[position++] = (uint8_t)(index >> 8);
      } else {
        dst[low + pixel] = (uint8_t)index;
        dst[high + pixel] = (uint8_t)(index >> 8);
      }
    }
  }
  if (layout == CPAL_PLANAR_SPLIT16)
    position += index_bytes;
  payload->size = position;
  return position == total;
}

static int try_cpal_planar_lzms(Candidate *best, const Buf *indices,
                                const uint16_t *raw_ids, const Buf *palette,
                                uint32_t width, uint32_t height, int index_bits,
                                uint32_t palette_count) {
  if (best->mode != MODE_CPAL || palette_count <= 256u ||
      palette_count > 65536u)
    return 1;
  size_t incumbent = cand_size(best);
  size_t best_proxy = SIZE_MAX;
  unsigned best_layout = CPAL_PLANAR_LAYOUT_COUNT;
  for (unsigned layout = 0; layout < CPAL_PLANAR_LAYOUT_COUNT; ++layout) {
    Buf payload = {0};
    if (!make_cpal_planar_payload(indices, raw_ids, palette, width, height,
                                  index_bits, palette_count, layout,
                                  &payload)) {
      buf_free(&payload);
      return 0;
    }
    if (layout == CPAL_PLANAR_INTERLEAVED16) {
      uint64_t incumbent_payload = best->payload_size;
      uint64_t allowance =
          incumbent_payload / 5u * 3u + (incumbent_payload % 5u) * 3u / 5u;
      if (!incumbent_payload ||
          ((uint64_t)payload.size > incumbent_payload &&
           (uint64_t)payload.size - incumbent_payload > allowance)) {
#ifdef QLIC_STREAM_TRACE
        if (qlic_candidate_trace_enabled())
          fprintf(stderr,
                  "candidate-cpal-planar-skip reason=decode-work "
                  "payload=%zu incumbent-payload=%llu allowance=%llu\n",
                  payload.size, (unsigned long long)incumbent_payload,
                  (unsigned long long)allowance);
#endif
        buf_free(&payload);
        return 1;
      }
    }
    size_t proxy_total = SIZE_MAX;
    int worth_trying = 0;
    if (!repeat_lzms_proxy(&payload, NULL, MODE_CPAL, TRANSFORM_CPAL_PLANAR,
                           incumbent, &proxy_total, &worth_trying, 1)) {
      buf_free(&payload);
      return 0;
    }
#ifdef QLIC_STREAM_TRACE
    if (qlic_candidate_trace_enabled())
      fprintf(stderr,
              "candidate-cpal-planar layout=%u colors=%u payload=%zu "
              "proxy-total=%zu incumbent=%zu proxy-worth=%d\n",
              layout, palette_count, payload.size, proxy_total, incumbent,
              worth_trying);
#endif
    if (proxy_total < best_proxy) {
      best_proxy = proxy_total;
      best_layout = layout;
    }
    buf_free(&payload);
  }

  /* The development and SHA-256 holdout split were both lossless at this
     high-precision XPRESS gate.  The completed LZMS file still has to win. */
  size_t allowance = incumbent / 10u;
  if (best_layout == CPAL_PLANAR_LAYOUT_COUNT ||
      (best_proxy > incumbent && best_proxy - incumbent > allowance))
    return 1;

  Buf payload = {0};
  if (!make_cpal_planar_payload(indices, raw_ids, palette, width, height,
                                index_bits, palette_count, best_layout,
                                &payload)) {
    buf_free(&payload);
    return 0;
  }
  int ok = try_pay_repeat_lzms_backend(best, &payload, NULL, MODE_CPAL,
                                       TRANSFORM_CPAL_PLANAR, index_bits,
                                       palette_count, 1u);
  buf_free(&payload);
  return ok;
}
#endif

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
/* Return -1 when the deliberate payload limit is reached, 0 on failure, and
   1 after appending. The limit bounds both live memory and backend work. */
static int buf_append_limited(Buf *buf, const void *data, size_t size,
                              size_t limit) {
  if (buf->size > limit || size > limit - buf->size)
    return -1;
  return buf_append(buf, data, size);
}

static int buf_varint_limited(Buf *buf, size_t value, size_t limit) {
  uint8_t bytes[(sizeof(size_t) * CHAR_BIT + 6u) / 7u];
  size_t count = 0;
  do {
    uint8_t byte = (uint8_t)(value & 127u);
    value >>= 7;
    if (value)
      byte |= 128u;
    bytes[count++] = byte;
  } while (value);
  return buf_append_limited(buf, bytes, count, limit);
}

/* Build the existing CPAL payload directly from a proven palette. Unlike
   mk_pal, this bounded path never retains one uint16_t index per pixel or both
   packed and run representations. */
static int make_exact_cpal_payload(const Image *im, uint32_t exact_count,
                                   int hot_run_order, Buf *payload,
                                   int *index_bits, int *transform) {
  size_t pixels = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels))
    return 0;
  if (exact_count < 2u || exact_count > 4096u ||
      pixels > REPEAT_LZMS_EXACT_PALETTE_MAX_PIXELS)
    return -1;

  size_t cap = np2((size_t)exact_count * 4u);
  MapEntry *colors = (MapEntry *)calloc(cap, sizeof(MapEntry));
  uint32_t *run_counts =
      hot_run_order
          ? (uint32_t *)calloc((size_t)exact_count, sizeof(uint32_t))
          : NULL;
  if (!colors || (hot_run_order && !run_counts)) {
    free(colors);
    free(run_counts);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  Buf palette = {0};
  uint32_t count = 0;
  uint32_t previous_value = UINT32_MAX;
  int result = 0;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t *pixel = im->rgba + i * 4u;
    uint32_t value = 0;
    if (!map_put(colors, cap - 1u, ckey(pixel), &value, 0, 0)) {
      if (count >= exact_count ||
          !map_put(colors, cap - 1u, ckey(pixel), &value, 1, count) ||
          !buf_append(&palette, pixel, 4u))
        goto done;
      ++count;
    }
    if (hot_run_order && value != previous_value)
      ++run_counts[value];
    previous_value = value;
  }
  if (count != exact_count) {
    set_err("internal exact palette mismatch");
    goto done;
  }

  uint16_t *old_to_new =
      (uint16_t *)xmalloc((size_t)exact_count * sizeof(uint16_t));
  if (!old_to_new)
    goto done;
  int sorted = hot_run_order
                   ? sort_palette_map_by_hot_runs(&palette, old_to_new,
                                                  run_counts, exact_count)
                   : sort_palette_map(&palette, old_to_new, exact_count);
  if (!sorted) {
    free(old_to_new);
    goto done;
  }
  for (size_t i = 0; i < cap; ++i) {
    if (colors[i].used)
      colors[i].value = old_to_new[colors[i].value];
  }
  free(old_to_new);

  *index_bits = pal_bits(exact_count);
  *transform =
      exact_count > 256u ? TRANSFORM_CPAL_DELTA : TRANSFORM_IDENTITY_RAW;
  if (*transform == TRANSFORM_CPAL_DELTA) {
    for (size_t i = 0; i < palette.size; ++i) {
      uint8_t byte = i < 4u ? palette.data[i]
                            : (uint8_t)(palette.data[i] - palette.data[i - 4u]);
      int appended = buf_append_limited(payload, &byte, 1u,
                                        REPEAT_LZMS_EXACT_PALETTE_MAX_PAYLOAD);
      if (appended <= 0) {
        result = appended;
        goto done;
      }
    }
  } else {
    result = buf_append_limited(payload, palette.data, palette.size,
                                REPEAT_LZMS_EXACT_PALETTE_MAX_PAYLOAD);
    if (result <= 0)
      goto done;
  }

  if (exact_count <= 256u) {
    size_t row_bytes = row_pack(im->width, *index_bits);
    size_t index_bytes = 0;
    if (!mulok(row_bytes, (size_t)im->height, &index_bytes))
      goto done;
    if (payload->size > REPEAT_LZMS_EXACT_PALETTE_MAX_PAYLOAD ||
        index_bytes > REPEAT_LZMS_EXACT_PALETTE_MAX_PAYLOAD - payload->size) {
      result = -1;
      goto done;
    }
    if (!buf_reserve(payload, index_bytes))
      goto done;
    for (uint32_t y = 0; y < im->height; ++y) {
      uint32_t accumulator = 0;
      int used = 0;
      for (uint32_t x = 0; x < im->width; ++x) {
        size_t pixel_index = (size_t)y * im->width + x;
        uint32_t value = 0;
        if (!map_put(colors, cap - 1u, ckey(im->rgba + pixel_index * 4u),
                     &value, 0, 0)) {
          set_err("internal exact palette lookup error");
          goto done;
        }
        if (*index_bits == 8) {
          payload->data[payload->size++] = (uint8_t)value;
        } else {
          accumulator |= value << used;
          used += *index_bits;
          while (used >= 8) {
            payload->data[payload->size++] = (uint8_t)accumulator;
            accumulator >>= 8;
            used -= 8;
          }
        }
      }
      if (used)
        payload->data[payload->size++] = (uint8_t)accumulator;
    }
  } else {
    size_t position = 0;
    while (position < pixels) {
      uint32_t value = 0;
      if (!map_put(colors, cap - 1u, ckey(im->rgba + position * 4u), &value, 0,
                   0)) {
        set_err("internal exact palette lookup error");
        goto done;
      }
      size_t run = 1;
      while (position + run < pixels) {
        uint32_t next = 0;
        if (!map_put(colors, cap - 1u, ckey(im->rgba + (position + run) * 4u),
                     &next, 0, 0)) {
          set_err("internal exact palette lookup error");
          goto done;
        }
        if (next != value)
          break;
        ++run;
      }
      int appended = buf_varint_limited(payload, run - 1u,
                                        REPEAT_LZMS_EXACT_PALETTE_MAX_PAYLOAD);
      if (appended > 0)
        appended = buf_varint_limited(payload, value,
                                      REPEAT_LZMS_EXACT_PALETTE_MAX_PAYLOAD);
      if (appended <= 0) {
        result = appended;
        goto done;
      }
      position += run;
    }
  }
  result = 1;

done:
  buf_free(&palette);
  free(colors);
  free(run_counts);
  return result;
}

static int try_exact_palette_repeat_lzms(Candidate *best, const Image *im,
                                         uint32_t exact_count) {
  int order_count = exact_count > 256u ? 2 : 1;
  for (int order = 0; order < order_count; ++order) {
    Buf payload = {0};
    int index_bits = 0;
    int transform = 0;
    int built = make_exact_cpal_payload(im, exact_count, order, &payload,
                                        &index_bits, &transform);
    if (built < 0) {
#ifdef QLIC_STREAM_TRACE
      if (qlic_candidate_trace_enabled())
        fprintf(stderr,
                "candidate-repeat-exact-palette-skip reason=payload-limit "
                "order=%s colors=%u partial-payload=%zu limit=%u\n",
                order ? "hot-runs" : "lexical", exact_count, payload.size,
                (unsigned)REPEAT_LZMS_EXACT_PALETTE_MAX_PAYLOAD);
#endif
      buf_free(&payload);
      continue;
    }
    if (!built) {
      buf_free(&payload);
      return 0;
    }
#ifdef QLIC_STREAM_TRACE
    if (qlic_candidate_trace_enabled())
      fprintf(stderr,
              "candidate-repeat-exact-palette order=%s colors=%u payload=%zu "
              "incumbent=%zu\n",
              order ? "hot-runs" : "lexical", exact_count, payload.size,
              cand_size(best));
#endif
    int ok = try_pay_repeat_lzms(best, &payload, NULL, MODE_CPAL, transform,
                                 index_bits, exact_count);
    buf_free(&payload);
    if (!ok)
      return 0;
  }
  return 1;
}

enum {
  TILE_PALETTE_MIN_PIXELS = 32768,
  TILE_PALETTE_MAX_PIXELS = 2000000,
  TILE_PALETTE_MAX_COLORS = 65536,
  TILE_PALETTE_MAX_PAYLOAD = 2 * 1024 * 1024,
  TILE_PALETTE_MIN_LOG = 3,
  TILE_PALETTE_MAX_LOG = 6
};

typedef struct {
  uint32_t old_index;
  uint32_t frequency;
  uint8_t color[4];
} TilePaletteSort;

typedef struct {
  Buf payload;
  size_t proxy_total;
} TilePaletteRank;

static int tile_palette_sort_cmp(const void *ap, const void *bp) {
  const TilePaletteSort *a = (const TilePaletteSort *)ap;
  const TilePaletteSort *b = (const TilePaletteSort *)bp;
  if (a->frequency != b->frequency)
    return a->frequency > b->frequency ? -1 : 1;
  for (int i = 0; i < 4; ++i) {
    if (a->color[i] != b->color[i])
      return a->color[i] < b->color[i] ? -1 : 1;
  }
  return 0;
}

static int tile_palette_u16_cmp(const void *ap, const void *bp) {
  uint16_t a = *(const uint16_t *)ap;
  uint16_t b = *(const uint16_t *)bp;
  return a < b ? -1 : a > b ? 1 : 0;
}

/* Sixteen fixed 8x8 patches cheaply distinguish locally bounded screen and
   texture structure from photographic color churn.  This is only a work
   gate: completed compressed bytes still decide whether the stream wins. */
static unsigned tile_palette_local_color_sum(const Image *im) {
  uint32_t patch_w = im->width < 8u ? im->width : 8u;
  uint32_t patch_h = im->height < 8u ? im->height : 8u;
  uint32_t last_x = im->width - patch_w;
  uint32_t last_y = im->height - patch_h;
  unsigned total = 0;
  for (unsigned gy = 0; gy < 4u; ++gy) {
    uint32_t y0 = (uint32_t)(((uint64_t)gy * last_y) / 3u);
    for (unsigned gx = 0; gx < 4u; ++gx) {
      uint32_t x0 = (uint32_t)(((uint64_t)gx * last_x) / 3u);
      uint32_t colors[64];
      unsigned count = 0;
      for (uint32_t y = 0; y < patch_h; ++y) {
        const uint8_t *row =
            im->rgba + ((size_t)(y0 + y) * im->width + x0) * 4u;
        for (uint32_t x = 0; x < patch_w; ++x) {
          uint32_t color = ckey(row + (size_t)x * 4u);
          unsigned i = 0;
          while (i < count && colors[i] != color)
            ++i;
          if (i == count)
            colors[count++] = color;
        }
      }
      total += count;
    }
  }
  return total;
}

/* Build one frequency-ordered global palette and a compact image-sized index
   plane. Natural images stop as soon as the exact palette exceeds the bound. */
static int make_tile_palette_state(const Image *im, Buf *palette,
                                   uint16_t **ids_out, uint32_t *count_out) {
  size_t pixels = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels))
    return 0;
  if (pixels < TILE_PALETTE_MIN_PIXELS || pixels > TILE_PALETTE_MAX_PIXELS)
    return -1;

  size_t cap = np2((size_t)TILE_PALETTE_MAX_COLORS * 4u);
  MapEntry *map = (MapEntry *)calloc(cap, sizeof(MapEntry));
  uint32_t *frequency =
      (uint32_t *)calloc(TILE_PALETTE_MAX_COLORS, sizeof(uint32_t));
  uint16_t *ids = (uint16_t *)xmalloc(pixels * sizeof(uint16_t));
  if (!map || !frequency || !ids) {
    free(map);
    free(frequency);
    free(ids);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }

  uint32_t count = 0;
  int result = 0;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t *pixel = im->rgba + i * 4u;
    uint32_t value = 0;
    if (!map_put(map, cap - 1u, ckey(pixel), &value, 0, 0)) {
      if (count >= TILE_PALETTE_MAX_COLORS) {
        result = -1;
        goto done;
      }
      value = count;
      if (!map_put(map, cap - 1u, ckey(pixel), &value, 1, value) ||
          !buf_append(palette, pixel, 4u)) {
        set_err("internal tile-palette map error");
        goto done;
      }
      ++count;
    }
    ids[i] = (uint16_t)value;
    ++frequency[value];
  }
  if (count < 2u) {
    result = -1;
    goto done;
  }

  TilePaletteSort *order =
      (TilePaletteSort *)xmalloc((size_t)count * sizeof(TilePaletteSort));
  uint16_t *old_to_new = (uint16_t *)xmalloc((size_t)count * sizeof(uint16_t));
  uint8_t *ordered_palette = (uint8_t *)xmalloc((size_t)count * 4u);
  if (!order || !old_to_new || !ordered_palette) {
    free(order);
    free(old_to_new);
    free(ordered_palette);
    goto done;
  }
  for (uint32_t i = 0; i < count; ++i) {
    order[i].old_index = i;
    order[i].frequency = frequency[i];
    memcpy(order[i].color, palette->data + (size_t)i * 4u, 4u);
  }
  qsort(order, count, sizeof(TilePaletteSort), tile_palette_sort_cmp);
  for (uint32_t i = 0; i < count; ++i) {
    old_to_new[order[i].old_index] = (uint16_t)i;
    memcpy(ordered_palette + (size_t)i * 4u, order[i].color, 4u);
  }
  memcpy(palette->data, ordered_palette, (size_t)count * 4u);
  for (size_t i = 0; i < pixels; ++i)
    ids[i] = old_to_new[ids[i]];
  free(order);
  free(old_to_new);
  free(ordered_palette);
  *ids_out = ids;
  *count_out = count;
  result = 1;

done:
  free(map);
  free(frequency);
  if (result <= 0) {
    free(ids);
    buf_free(palette);
  }
  return result;
}

static int make_tile_palette_payload(const Image *im, const Buf *palette,
                                     const uint16_t *ids,
                                     uint32_t palette_count, unsigned tile_log,
                                     Buf *payload) {
  uint32_t tile_size = 1u << tile_log;
  size_t max_tile_pixels = (size_t)tile_size * tile_size;
  uint16_t *local = (uint16_t *)xmalloc(max_tile_pixels * sizeof(uint16_t));
  uint16_t *rank =
      (uint16_t *)xmalloc((size_t)palette_count * sizeof(uint16_t));
  if (!local || !rank) {
    free(local);
    free(rank);
    return 0;
  }
  memset(rank, 0xff, (size_t)palette_count * sizeof(uint16_t));
  uint8_t log_byte = (uint8_t)tile_log;
  int appended =
      buf_append_limited(payload, &log_byte, 1u, TILE_PALETTE_MAX_PAYLOAD);
  if (appended > 0)
    appended = buf_append_limited(payload, palette->data, palette->size,
                                  TILE_PALETTE_MAX_PAYLOAD);
  if (appended <= 0) {
    free(local);
    free(rank);
    return appended;
  }

  for (uint32_t y0 = 0; y0 < im->height; y0 += tile_size) {
    uint32_t tile_height =
        im->height - y0 < tile_size ? im->height - y0 : tile_size;
    for (uint32_t x0 = 0; x0 < im->width; x0 += tile_size) {
      uint32_t tile_width =
          im->width - x0 < tile_size ? im->width - x0 : tile_size;
      size_t local_count = 0;
      for (uint32_t y = 0; y < tile_height; ++y) {
        const uint16_t *source = ids + (size_t)(y0 + y) * im->width + x0;
        for (uint32_t x = 0; x < tile_width; ++x) {
          uint16_t id = source[x];
          if (rank[id] == UINT16_MAX) {
            rank[id] = 0;
            local[local_count++] = id;
          }
        }
      }
      qsort(local, local_count, sizeof(uint16_t), tile_palette_u16_cmp);
      appended = buf_varint_limited(payload, local_count - 1u,
                                    TILE_PALETTE_MAX_PAYLOAD);
      size_t previous = 0;
      for (size_t i = 0; appended > 0 && i < local_count; ++i) {
        size_t gap = i ? (size_t)local[i] - previous - 1u : local[i];
        appended = buf_varint_limited(payload, gap, TILE_PALETTE_MAX_PAYLOAD);
        previous = local[i];
        rank[local[i]] = (uint16_t)i;
      }
      if (appended <= 0) {
        free(local);
        free(rank);
        return appended;
      }

      int local_bits = pal_bits((uint32_t)local_count);
      uint32_t accumulator = 0;
      int used = 0;
      for (uint32_t y = 0; y < tile_height; ++y) {
        const uint16_t *source = ids + (size_t)(y0 + y) * im->width + x0;
        for (uint32_t x = 0; x < tile_width; ++x) {
          accumulator |= (uint32_t)rank[source[x]] << used;
          used += local_bits;
          while (used >= 8) {
            uint8_t byte = (uint8_t)accumulator;
            appended = buf_append_limited(payload, &byte, 1u,
                                          TILE_PALETTE_MAX_PAYLOAD);
            if (appended <= 0) {
              free(local);
              free(rank);
              return appended;
            }
            accumulator >>= 8;
            used -= 8;
          }
        }
      }
      if (used) {
        uint8_t byte = (uint8_t)accumulator;
        appended =
            buf_append_limited(payload, &byte, 1u, TILE_PALETTE_MAX_PAYLOAD);
        if (appended <= 0) {
          free(local);
          free(rank);
          return appended;
        }
      }
      for (size_t i = 0; i < local_count; ++i)
        rank[local[i]] = UINT16_MAX;
    }
  }
  free(local);
  free(rank);
  return 1;
}

static void tile_palette_rank_insert(TilePaletteRank ranked[2], Buf *payload,
                                     size_t proxy_total) {
  TilePaletteRank incoming = {*payload, proxy_total};
  memset(payload, 0, sizeof(*payload));
  if (proxy_total < ranked[0].proxy_total) {
    buf_free(&ranked[1].payload);
    ranked[1] = ranked[0];
    ranked[0] = incoming;
  } else if (proxy_total < ranked[1].proxy_total) {
    buf_free(&ranked[1].payload);
    ranked[1] = incoming;
  } else {
    buf_free(&incoming.payload);
  }
}

static int try_tile_palette_lzms(Candidate *best, const Image *im) {
  size_t pixels = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels))
    return 0;
  if (pixels < TILE_PALETTE_MIN_PIXELS || pixels > TILE_PALETTE_MAX_PIXELS)
    return 1;
  size_t incumbent = cand_size(best);
  unsigned local_color_sum = tile_palette_local_color_sum(im);
  int preliminary_route =
      (incumbent < 30000u && local_color_sum <= 24u * 16u) ||
      (incumbent >= 30000u && incumbent < 75000u &&
       local_color_sum <= 6u * 16u) ||
      (incumbent >= 75000u && incumbent < 150000u &&
       local_color_sum <= 40u * 16u) ||
      (incumbent >= 150000u && local_color_sum <= 32u * 16u);
  if (!preliminary_route)
    return 1;

  Buf palette = {0};
  uint16_t *ids = NULL;
  uint32_t palette_count = 0;
  int state = make_tile_palette_state(im, &palette, &ids, &palette_count);
  if (state <= 0) {
    free(ids);
    return state < 0 ? 1 : 0;
  }

  int large_palette_route = palette_count > 4096u && incumbent >= 150000u &&
                            local_color_sum <= 32u * 16u;
  if (palette_count > 4096u && !large_palette_route) {
    buf_free(&palette);
    free(ids);
    return 1;
  }

  int ok = 1;
  /* Preserve the endpoint result before considering a middle scale. Ranking
     all four scales together can evict a completed endpoint when XPRESS and
     LZMS disagree. Try the middle pair only after the two-scale tournament has
     updated the current result. */
  const unsigned endpoint_logs[2] = {3u, 6u};
  const unsigned middle_logs[2] = {4u, 5u};
  const unsigned large_logs[1] = {5u};
  int middle_scales = qlic_tile_palette_middle_scales_enabled();
  unsigned group_count = !large_palette_route && middle_scales ? 2u : 1u;
  int index_bits = pal_bits(palette_count);
  for (unsigned group = 0; group < group_count && ok; ++group) {
    const unsigned *tile_logs = large_palette_route ? large_logs
                                : group             ? middle_logs
                                                    : endpoint_logs;
    unsigned tile_log_count = large_palette_route ? 1u : 2u;
    TilePaletteRank ranked[2] = {{{0}, SIZE_MAX}, {{0}, SIZE_MAX}};
    size_t group_incumbent = cand_size(best);
    int group_incumbent_cpal = best->mode == MODE_CPAL;
    uint64_t group_payload_limit = best->payload_size;
    for (unsigned trial = 0; trial < tile_log_count; ++trial) {
      unsigned tile_log = tile_logs[trial];
      Buf payload = {0};
      int built = make_tile_palette_payload(im, &palette, ids, palette_count,
                                            tile_log, &payload);
      if (built < 0) {
        buf_free(&payload);
        continue;
      }
      if (!built) {
        buf_free(&payload);
        ok = 0;
        break;
      }
      size_t proxy_total = SIZE_MAX;
      int worth_trying = 0;
      if (!repeat_lzms_proxy(&payload, NULL, MODE_CPAL, TRANSFORM_CPAL_TILES,
                             group_incumbent, &proxy_total, &worth_trying, 1)) {
        buf_free(&payload);
        ok = 0;
        break;
      }
#ifdef QLIC_STREAM_TRACE
      if (qlic_candidate_trace_enabled())
        fprintf(stderr,
                "candidate-tile-palette tile-log=%u colors=%u payload=%zu "
                "proxy-total=%zu incumbent=%zu try=%d\n",
                tile_log, palette_count, payload.size, proxy_total,
                group_incumbent, worth_trying);
#endif
      if (worth_trying)
        tile_palette_rank_insert(ranked, &payload, proxy_total);
      buf_free(&payload);
    }
    int pay_count = group ? 1 : 2;
    for (int i = 0; i < pay_count && ok; ++i) {
      if (!ranked[i].payload.data)
        continue;
      /* Compare decoded work against the incumbent that this group can
         replace, including a tile endpoint selected just above. */
      if (group_incumbent_cpal &&
          ranked[i].payload.size > group_payload_limit) {
        continue;
      }
      /* On the optional middle scale, a byte-level tie-break is not worth a
         different tile traversal.  A retained 2.7 KiB case saved 16 bytes but
         decoded 1.56% slower; 32 bytes removes that bad trade while leaving
         every material middle-scale win intact. */
      size_t minimum_saving = group ? 32u : 1u;
      if (!try_pay_repeat_lzms_backend(
              best, &ranked[i].payload, NULL, MODE_CPAL, TRANSFORM_CPAL_TILES,
              index_bits, palette_count, minimum_saving)) {
        ok = 0;
        break;
      }
    }
    buf_free(&ranked[0].payload);
    buf_free(&ranked[1].payload);
  }
  if (ok && qlic_cpal_planar_enabled() &&
      !try_cpal_planar_lzms(best, NULL, ids, &palette, im->width, im->height,
                            index_bits, palette_count))
    ok = 0;
  buf_free(&palette);
  free(ids);
  return ok;
}
#endif

static int try_cpal(Candidate *best, const Buf *indices, const Buf *palette,
                    int transform, int index_bits, uint32_t palette_count,
                    uint32_t width, uint32_t height, int *codecs,
                    int codec_count) {
#ifndef QLIC_STREAM_TRACE
  (void)width;
  (void)height;
#endif
  Buf payload = {0};
  if (!make_cpal_payload(indices, palette, transform, &payload)) {
    buf_free(&payload);
    return 0;
  }
#ifdef QLIC_STREAM_TRACE
  if (transform == TRANSFORM_IDENTITY_RAW && qlic_cpal_trace_enabled() &&
      !trace_cpal_candidates(&payload, palette, indices, index_bits,
                             palette_count, width, height)) {
    buf_free(&payload);
    return 0;
  }
#endif
  int ok = try_pay(best, &payload, NULL, MODE_CPAL, transform, index_bits,
                   palette_count, codecs, codec_count);
  buf_free(&payload);
  return ok;
}

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
static int try_cpal_repeat_lzms(Candidate *best, const Buf *indices,
                                const Buf *palette, int transform,
                                int index_bits, uint32_t palette_count) {
  Buf payload = {0};
  if (!make_cpal_payload(indices, palette, transform, &payload)) {
    buf_free(&payload);
    return 0;
  }
  int ok = try_pay_repeat_lzms(best, &payload, NULL, MODE_CPAL, transform,
                               index_bits, palette_count);
  buf_free(&payload);
  return ok;
}
#endif

static int try_rows(Candidate *best, const uint8_t *samples, size_t row_bytes,
                    uint32_t height, int predictor_bpp, const Buf *palette,
                    int mode, int transform, int index_bits,
                    uint32_t palette_count, int *codecs, int codec_count) {
  Buf filtered = {0};
  if (!mk_frows(samples, row_bytes, height, predictor_bpp, 1, &filtered))
    return 0;
  int ok = try_pay(best, &filtered, palette, mode, transform, index_bits,
                   palette_count, codecs, codec_count);
  buf_free(&filtered);
  return ok;
}

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
static int try_rows_repeat_lzms(Candidate *best, const uint8_t *samples,
                                size_t row_bytes, uint32_t height,
                                int predictor_bpp, int mode, int transform,
                                int filter_search) {
  Buf filtered = {0};
  if (!mk_frows(samples, row_bytes, height, predictor_bpp, filter_search,
                &filtered))
    return 0;
  int ok = try_pay_repeat_lzms(best, &filtered, NULL, mode, transform, 0, 0);
  buf_free(&filtered);
  return ok;
}
#endif

static int try_pstream_one(Candidate *best, const Buf *payload,
                           const Buf *palette, uint32_t zwidth, uint32_t height,
                           int search, int transform, int index_bits,
                           uint32_t palette_count, int *codecs,
                           int codec_count) {
  uint8_t *data = NULL;
  size_t size = 0;
  int err = stream_encode_threads(payload->data, zwidth, height, 1, search,
                                  g_threads, &data, &size);
  if (err == STREAM_E_DIM)
    return 1;
  if (err != STREAM_OK) {
    set_err_status(stream_failure_status(err, 0),
                   "palette stream encode failed: %s", stream_strerror(err));
    return 0;
  }
  Buf z = {0};
  z.data = data;
  z.size = size;
  z.cap = size;
  int ok = try_pay(best, &z, palette, MODE_PSTREAM, transform, index_bits,
                   palette_count, codecs, codec_count);
  stream_free(data);
  return ok;
}

static int try_pstream(Candidate *best, const Buf *indices, size_t row_bytes,
                       uint32_t height, int predictor_bpp, int search,
                       const Buf *palette, int index_bits,
                       uint32_t palette_count, int *codecs, int codec_count) {
  if (row_bytes > UINT32_MAX) {
    set_err("palette row is too wide for palette stream");
    return 0;
  }
  if (!try_pstream_one(best, indices, palette, (uint32_t)row_bytes, height,
                       search, TRANSFORM_IDENTITY_RAW, index_bits,
                       palette_count, codecs, codec_count))
    return 0;
  Buf filtered = {0};
  if (!mk_frows(indices->data, row_bytes, height, predictor_bpp, search,
                &filtered))
    return 0;
  if (row_bytes + 1u > UINT32_MAX) {
    buf_free(&filtered);
    set_err("palette filtered row is too wide for palette stream");
    return 0;
  }
  int ok = try_pstream_one(best, &filtered, palette, (uint32_t)(row_bytes + 1u),
                           height, search, TRANSFORM_IDENTITY, index_bits,
                           palette_count, codecs, codec_count);
  buf_free(&filtered);
  return ok;
}

static int try_rle(Candidate *best, const Buf *samples, const Buf *palette,
                   int mode, int transform, int index_bits,
                   uint32_t palette_count, int *codecs, int codec_count) {
  Buf rle = {0};
  if (!rle_encode(samples->data, samples->size, &rle)) {
    buf_free(&rle);
    return 0;
  }
  int ok = try_pay(best, &rle, palette, mode, transform, index_bits,
                   palette_count, codecs, codec_count);
  buf_free(&rle);
  return ok;
}

static int try_stream_bytes(Candidate *best, const uint8_t *pix, uint32_t width,
                            uint32_t height, int channels, size_t pixel_stride,
                            int search, int *codecs, int codec_count) {
  uint8_t *data = NULL;
  size_t size = 0;
  int err = stream_encode_strided_threads(
      pix, width, height, channels, pixel_stride, search,
      search ? g_threads : 1u, &data, &size);
  if (err == STREAM_E_DIM)
    return 1;
  if (err != STREAM_OK) {
    set_err_status(stream_failure_status(err, 0),
                   "native stream encode failed: %s", stream_strerror(err));
    return 0;
  }
  Buf z = {0};
  z.data = data;
  z.size = size;
  z.cap = size;
  int ok;
  /* outer native headers stay identity because QST1 owns transform selection */
  if (codec_count == 1 && codecs[0] == CODEC_STORE) {
    ok = try_store_owned(best, &z, MODE_NATIVE, TRANSFORM_IDENTITY, 0, 0);
    data = z.data;
  } else {
    ok = try_pay(best, &z, NULL, MODE_NATIVE, TRANSFORM_IDENTITY, 0, 0, codecs,
                 codec_count);
  }
  stream_free(data);
  return ok;
}

static int try_stream(Candidate *best, const Image *im, int mode, int search,
                      int *codecs, int codec_count) {
  if (mode == MODE_GRAYA || mode == MODE_RGBA) {
    return try_stream_bytes(best, im->rgba, im->width, im->height, 4, 4u,
                            search, codecs, codec_count);
  }
  if (mode == MODE_GRAY) {
    return try_stream_bytes(best, im->rgba, im->width, im->height, 1, 4u,
                            search, codecs, codec_count);
  }
  return try_stream_bytes(best, im->rgba, im->width, im->height, 3, 4u, search,
                          codecs, codec_count);
}

static int try_alpha_palette(Candidate *best, const Image *im, int *codecs,
                             int codec_count) {
  Buf palette = {0};
  Buf indices = {0};
  Buf index_runs = {0};
  uint16_t *raw_ids = NULL;
  int index_bits = 0;
  uint32_t palette_count = 0;
  int result = mk_pal(im, 65536u, &palette, &indices, &index_runs, NULL,
                      &index_bits, &palette_count, &raw_ids);
  int ok = result >= 0;
  if (result > 0 && palette_count > 1u) {
    int large = palette_count > 256u;
    ok = try_cpal(best, large ? &index_runs : &indices, &palette,
                  large ? TRANSFORM_CPAL_DELTA : TRANSFORM_IDENTITY_RAW,
                  index_bits, palette_count, im->width, im->height, codecs,
                  codec_count);
#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
    if (ok && large && qlic_cpal_planar_enabled())
      ok = try_cpal_planar_lzms(best, NULL, raw_ids, &palette, im->width,
                                im->height, index_bits, palette_count);
#endif
  }
  free(raw_ids);
  buf_free(&index_runs);
  buf_free(&indices);
  buf_free(&palette);
  return ok;
}

static int try_repeat_lzms(Candidate *best, const Image *im, int mode,
                           size_t pixels, uint32_t exact_palette_count) {
#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
  int exact_palette = exact_palette_count > 1u;
  if (mode != MODE_GRAY && pixels <= REPEAT_LZMS_PALETTE_MAX_PIXELS) {
    Buf palette = {0};
    Buf indices = {0};
    Buf index_runs = {0};
    int index_bits = 0;
    uint32_t palette_count = 0;
    uint16_t *raw_ids = NULL;
    int expect_large = exact_palette && exact_palette_count > 256u;
    uint32_t palette_limit = exact_palette ? exact_palette_count : 65536u;
    int result =
        mk_pal(im, palette_limit, &palette, expect_large ? NULL : &indices,
               exact_palette && !expect_large ? NULL : &index_runs, NULL,
               &index_bits, &palette_count, &raw_ids);
#ifdef QLIC_STREAM_TRACE
    if (qlic_candidate_trace_enabled())
      fprintf(stderr,
              "candidate-repeat-palette result=%d colors=%u indices=%zu "
              "index-runs=%zu pred-runs=0 incumbent=%zu\n",
              result, palette_count, indices.size, index_runs.size,
              cand_size(best));
#endif
    int ok = result >= 0;
    if (result > 0 && palette_count > 1u) {
      int large = palette_count > 256u;
      ok = try_cpal_repeat_lzms(best, large ? &index_runs : &indices, &palette,
                                large ? TRANSFORM_CPAL_DELTA
                                      : TRANSFORM_IDENTITY_RAW,
                                index_bits, palette_count);
      if (ok && large && qlic_cpal_planar_enabled())
        ok = try_cpal_planar_lzms(best, NULL, raw_ids, &palette, im->width,
                                  im->height, index_bits, palette_count);
    }
#ifdef QLIC_STREAM_TRACE
    if (qlic_candidate_trace_enabled())
      fprintf(stderr, "candidate-repeat-state name=palette mode=%d bytes=%zu\n",
              best->compressed ? best->mode : -1, cand_size(best));
#endif
    buf_free(&index_runs);
    buf_free(&indices);
    buf_free(&palette);
    free(raw_ids);
    if (!ok)
      return 0;
  }

  Buf samples = {0};
  int transform = mode == MODE_RGB || mode == MODE_RGBA ? TRANSFORM_GDELTA
                                                        : TRANSFORM_IDENTITY;
  size_t row_bytes = 0;
  if (!mk_samp(im, mode, transform, &samples) ||
      !mulok((size_t)im->width, (size_t)mbpp(mode), &row_bytes) ||
      !try_rows_repeat_lzms(best, samples.data, row_bytes, im->height,
                            mbpp(mode), mode, transform, 1)) {
    buf_free(&samples);
    return 0;
  }
#ifdef QLIC_STREAM_TRACE
  if (qlic_candidate_trace_enabled())
    fprintf(stderr, "candidate-repeat-state name=rows mode=%d bytes=%zu\n",
            best->compressed ? best->mode : -1, cand_size(best));
#endif
  buf_free(&samples);
#else
  (void)best;
  (void)im;
  (void)mode;
  (void)pixels;
  (void)exact_palette_count;
#endif
  return 1;
}

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
typedef struct {
  size_t proxy_total;
  int transform;
  int filter_search;
} TexturedRowTrial;

static void keep_textured_row_trial(TexturedRowTrial best[2],
                                    const TexturedRowTrial *trial) {
  if (trial->proxy_total < best[0].proxy_total) {
    best[1] = best[0];
    best[0] = *trial;
  } else if (trial->proxy_total < best[1].proxy_total) {
    best[1] = *trial;
  }
}

/* The textured-span signal is deliberately rare, so it can afford a richer
   CPU-native row tournament. XPRESS ranks two candidates; only those two pay
   for LZMS. Existing filter and color-transform syntax keeps decode unchanged.
 */
static int try_textured_row_tournament(Candidate *best, const Image *im,
                                       int mode, size_t row_bytes) {
  static const int color_transforms[] = {TRANSFORM_GDELTA, TRANSFORM_IDENTITY,
                                         TRANSFORM_RDELTA, TRANSFORM_BDELTA};
  static const int gray_transforms[] = {TRANSFORM_IDENTITY};
  int color = mode == MODE_RGB || mode == MODE_RGBA;
  const int *transforms = color ? color_transforms : gray_transforms;
  size_t transform_count =
      color ? sizeof(color_transforms) / sizeof(color_transforms[0])
            : sizeof(gray_transforms) / sizeof(gray_transforms[0]);
  TexturedRowTrial ranked[2] = {{SIZE_MAX, TRANSFORM_IDENTITY, 1},
                                {SIZE_MAX, TRANSFORM_IDENTITY, 1}};
  size_t incumbent = cand_size(best);
  for (size_t i = 0; i < transform_count; ++i) {
    Buf samples = {0};
    if (!mk_samp(im, mode, transforms[i], &samples)) {
      buf_free(&samples);
      return 0;
    }
    for (int filter_search = 1; filter_search <= 2; ++filter_search) {
      Buf filtered = {0};
      if (!mk_frows(samples.data, row_bytes, im->height, mbpp(mode),
                    filter_search, &filtered)) {
        buf_free(&filtered);
        buf_free(&samples);
        return 0;
      }
      size_t proxy_total = SIZE_MAX;
      int worth_trying = 0;
      int ok = repeat_lzms_proxy(&filtered, NULL, mode, transforms[i],
                                 incumbent, &proxy_total, &worth_trying, 1);
#ifdef QLIC_STREAM_TRACE
      if (qlic_candidate_trace_enabled())
        fprintf(stderr,
                "candidate-textured-row-proxy transform=%d filter-search=%d "
                "proxy-total=%zu incumbent=%zu try=%d\n",
                transforms[i], filter_search, proxy_total, incumbent,
                worth_trying);
#endif
      buf_free(&filtered);
      if (!ok) {
        buf_free(&samples);
        return 0;
      }
      if (worth_trying) {
        TexturedRowTrial trial = {proxy_total, transforms[i], filter_search};
        keep_textured_row_trial(ranked, &trial);
      }
    }
    buf_free(&samples);
  }
  for (size_t i = 0; i < 2u && ranked[i].proxy_total != SIZE_MAX; ++i) {
    Buf samples = {0};
    Buf filtered = {0};
    int ok = mk_samp(im, mode, ranked[i].transform, &samples) &&
             mk_frows(samples.data, row_bytes, im->height, mbpp(mode),
                      ranked[i].filter_search, &filtered) &&
             try_pay_repeat_lzms_backend(best, &filtered, NULL, mode,
                                         ranked[i].transform, 0, 0, 1u);
    buf_free(&filtered);
    buf_free(&samples);
    if (!ok)
      return 0;
  }
  return 1;
}
#endif

static int try_textured_repeat_lzms(Candidate *best, const Image *im,
                                    int mode) {
#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
  Buf samples = {0};
  int transform = mode == MODE_RGB || mode == MODE_RGBA ? TRANSFORM_GDELTA
                                                        : TRANSFORM_IDENTITY;
  int raw_transform =
      is_gd(transform) ? TRANSFORM_GDELTA_RAW : TRANSFORM_IDENTITY_RAW;
  size_t row_bytes = 0;
  int ok = mk_samp(im, mode, transform, &samples) &&
           mulok((size_t)im->width, (size_t)mbpp(mode), &row_bytes) &&
           try_pay_repeat_lzms(best, &samples, NULL, mode, raw_transform, 0, 0);
  buf_free(&samples);
  return ok && try_textured_row_tournament(best, im, mode, row_bytes);
#else
  (void)best;
  (void)im;
  (void)mode;
  return 1;
#endif
}

static uint8_t med_predict8(uint8_t left, uint8_t up, uint8_t upper_left) {
  int low = left < up ? left : up;
  int high = left > up ? left : up;
  int gradient = (int)left + (int)up - (int)upper_left;
  gradient = gradient < low ? low : gradient;
  gradient = gradient > high ? high : gradient;
  return (uint8_t)gradient;
}

static inline uint8_t planar_med_step(uint8_t *plane, size_t pos,
                                      size_t width) {
  uint8_t value =
      (uint8_t)(plane[pos] + med_predict8(plane[pos - 1u], plane[pos - width],
                                          plane[pos - width - 1u]));
  plane[pos] = value;
  return value;
}

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
enum { PLANAR_MED_MAX_PIXELS = 4000000 };

static uint8_t planar_blue_component(const uint8_t *pixel, int channel) {
  if (channel == 0)
    return pixel[2];
  if (channel == 1)
    return (uint8_t)(pixel[0] - pixel[2]);
  if (channel == 2)
    return (uint8_t)(pixel[1] - pixel[2]);
  return pixel[3];
}

/* Store one causal MED residual plane at a time. Reverse traversal lets the
   transform happen in place: every left/up source value is still untouched. */
static int make_planar_med_payload(const Image *im, int mode, Buf *payload) {
  size_t pixels = 0;
  size_t bytes = 0;
  int channels = mbpp(mode);
  if ((mode != MODE_RGB && mode != MODE_RGBA) ||
      !mulok((size_t)im->width, (size_t)im->height, &pixels) ||
      pixels > PLANAR_MED_MAX_PIXELS ||
      !mulok(pixels, (size_t)channels, &bytes))
    return 0;
  if (!buf_reserve(payload, bytes))
    return 0;
  payload->size = bytes;

  for (int channel = 0; channel < channels; ++channel) {
    uint8_t *plane = payload->data + pixels * (size_t)channel;
    for (size_t i = 0; i < pixels; ++i)
      plane[i] = planar_blue_component(im->rgba + i * 4u, channel);
    for (uint32_t y = im->height; y-- > 0;) {
      size_t row = (size_t)y * im->width;
      for (uint32_t x = im->width; x-- > 0;) {
        size_t pos = row + x;
        uint8_t left = x ? plane[pos - 1u] : 0u;
        uint8_t up = y ? plane[pos - im->width] : 0u;
        uint8_t upper_left = x && y ? plane[pos - im->width - 1u] : 0u;
        plane[pos] = (uint8_t)(plane[pos] - med_predict8(left, up, upper_left));
      }
    }
  }
  return 1;
}

/* Final byte-stream retry. Palette modes have lower reconstruction cost, so
   size alone cannot displace them. A 16% XPRESS-Huffman screen pays for LZMS on
   the rare planar candidates that can beat completed RGB/RGBA bytes. */
static int try_planar_med_lzms(Candidate *best, const Image *im) {
  if (!best->compressed || (best->mode != MODE_RGB && best->mode != MODE_RGBA))
    return 1;
  size_t pixels = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels) ||
      pixels > PLANAR_MED_MAX_PIXELS)
    return 1;
  int mode = scan_mode(im);
  if (mode != MODE_RGB && mode != MODE_RGBA)
    return 1;
  Buf payload = {0};
  int ok = make_planar_med_payload(im, mode, &payload) &&
           try_pay_repeat_lzms(best, &payload, NULL, mode,
                               TRANSFORM_BDELTA_PLANAR_MED, 0, 0);
  buf_free(&payload);
  return ok;
}
#endif

static int circ8(int a, int b) {
  int d = (a - b) & 255;
  return d > 128 ? 256 - d : d;
}

static int gmodel_slope(const uint8_t *g, uint32_t w, uint32_t h, int p,
                        int vertical) {
  uint64_t hist[256];
  memset(hist, 0, sizeof(hist));
  if ((!vertical && w <= (uint32_t)p) || (vertical && h <= (uint32_t)p))
    return 0;
  if (vertical) {
    for (uint32_t y = 0; y + (uint32_t)p < h; ++y) {
      const uint8_t *row = g + (size_t)y * w;
      const uint8_t *row2 = g + (size_t)(y + (uint32_t)p) * w;
      for (uint32_t x = 0; x < w; ++x)
        ++hist[(uint8_t)(row2[x] - row[x])];
    }
  } else {
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t *row = g + (size_t)y * w;
      for (uint32_t x = 0; x + (uint32_t)p < w; ++x)
        ++hist[(uint8_t)(row[x + (uint32_t)p] - row[x])];
    }
  }
  uint64_t best_cost = UINT64_MAX;
  int best = 0;
  for (int a = 0; a < 256; ++a) {
    int target = (a * p) & 255;
    uint64_t cost = 0;
    for (int d = 0; d < 256; ++d)
      cost += hist[d] * (uint64_t)circ8(d, target);
    if (cost < best_cost) {
      best_cost = cost;
      best = a;
    }
  }
  return best;
}

static int gmodel_phase(const uint8_t *g, uint32_t w, uint32_t h, int p, int a,
                        int b, uint8_t *phase) {
  size_t pn = (size_t)p * (size_t)p;
  uint32_t *cnt = (uint32_t *)calloc(pn * 256u, sizeof(*cnt));
  if (!cnt) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  for (uint32_t y = 0; y < h; ++y) {
    uint32_t py = (uint32_t)((int)(y % (uint32_t)p) * p);
    uint32_t by = (uint32_t)b * y;
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t ph = py + (x % (uint32_t)p);
      uint8_t v =
          (uint8_t)(g[(size_t)y * w + x] - (uint8_t)((uint32_t)a * x + by));
      ++cnt[(size_t)ph * 256u + v];
    }
  }
  for (size_t ph = 0; ph < pn; ++ph) {
    uint32_t best_n = 0;
    uint8_t best_v = 0;
    uint32_t *base = cnt + ph * 256u;
    for (int v = 0; v < 256; ++v) {
      if (base[v] > best_n) {
        best_n = base[v];
        best_v = (uint8_t)v;
      }
    }
    phase[ph] = best_v;
  }
  free(cnt);
  return 1;
}

static int gmodel_matches(const uint8_t *g, uint32_t w, uint32_t h, int p,
                          int a, int b, size_t *matches) {
  size_t pn = (size_t)p * (size_t)p;
  uint32_t *cnt = (uint32_t *)calloc(pn * 256u, sizeof(*cnt));
  if (!cnt) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  for (uint32_t y = 0; y < h; ++y) {
    uint32_t py = (uint32_t)((int)(y % (uint32_t)p) * p);
    uint32_t by = (uint32_t)b * y;
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t ph = py + (x % (uint32_t)p);
      uint8_t v =
          (uint8_t)(g[(size_t)y * w + x] - (uint8_t)((uint32_t)a * x + by));
      ++cnt[(size_t)ph * 256u + v];
    }
  }
  size_t total = 0;
  for (size_t ph = 0; ph < pn; ++ph) {
    uint32_t best = 0;
    const uint32_t *base = cnt + ph * 256u;
    for (int v = 0; v < 256; ++v)
      if (base[v] > best)
        best = base[v];
    total += best;
  }
  free(cnt);
  *matches = total;
  return 1;
}

static int gmodel_payload(const uint8_t *g, uint32_t w, uint32_t h, int p,
                          int bl, int a, int b, Buf *out) {
  size_t pixels = 0;
  if (!mulok((size_t)w, (size_t)h, &pixels))
    return 0;
  size_t pn = (size_t)p * (size_t)p;
  uint8_t *phase = (uint8_t *)xmalloc(pn);
  uint8_t *res = (uint8_t *)xmalloc(pixels);
  if (!phase || !res) {
    free(phase);
    free(res);
    return 0;
  }
  if (!gmodel_phase(g, w, h, p, a, b, phase)) {
    free(phase);
    free(res);
    return 0;
  }
  uint32_t bw = 0;
  uint32_t bh = 0;
  size_t blocks = 0;
  if (bl > 0) {
    bw = (w + (uint32_t)bl - 1u) / (uint32_t)bl;
    bh = (h + (uint32_t)bl - 1u) / (uint32_t)bl;
    if (!mulok((size_t)bw, (size_t)bh, &blocks)) {
      free(phase);
      free(res);
      return 0;
    }
  }
  size_t need = 0;
  if (!addok(4u, pn, &need) || !addok(need, blocks, &need) ||
      !addok(need, pixels, &need)) {
    free(phase);
    free(res);
    return 0;
  }
  if (!buf_reserve(out, need)) {
    free(phase);
    free(res);
    return 0;
  }
  if (!buf_u8(out, (uint8_t)p) || !buf_u8(out, (uint8_t)bl) ||
      !buf_u8(out, (uint8_t)a) || !buf_u8(out, (uint8_t)b) ||
      !buf_append(out, phase, pn)) {
    free(phase);
    free(res);
    return 0;
  }
  for (uint32_t y = 0; y < h; ++y) {
    uint32_t byv = (uint32_t)b * y;
    uint32_t py = (uint32_t)((int)(y % (uint32_t)p) * p);
    for (uint32_t x = 0; x < w; ++x) {
      size_t i = (size_t)y * w + x;
      uint8_t base =
          (uint8_t)((uint32_t)a * x + byv + phase[py + (x % (uint32_t)p)]);
      res[i] = (uint8_t)(g[i] - base);
    }
  }
  if (bl > 0) {
    uint8_t *bt = (uint8_t *)xmalloc(blocks);
    if (!bt) {
      free(phase);
      free(res);
      return 0;
    }
    for (uint32_t by = 0; by < bh; ++by) {
      for (uint32_t bx = 0; bx < bw; ++bx) {
        uint32_t hist[256];
        memset(hist, 0, sizeof(hist));
        uint32_t y1 = by * (uint32_t)bl;
        uint32_t y2 = y1 + (uint32_t)bl < h ? y1 + (uint32_t)bl : h;
        uint32_t x1 = bx * (uint32_t)bl;
        uint32_t x2 = x1 + (uint32_t)bl < w ? x1 + (uint32_t)bl : w;
        for (uint32_t yy = y1; yy < y2; ++yy) {
          const uint8_t *row = res + (size_t)yy * w;
          for (uint32_t xx = x1; xx < x2; ++xx)
            ++hist[row[xx]];
        }
        uint32_t best_n = 0;
        uint8_t best_v = 0;
        for (int v = 0; v < 256; ++v) {
          if (hist[v] > best_n) {
            best_n = hist[v];
            best_v = (uint8_t)v;
          }
        }
        bt[(size_t)by * bw + bx] = best_v;
        if (!buf_u8(out, best_v)) {
          free(bt);
          free(phase);
          free(res);
          return 0;
        }
      }
    }
    for (uint32_t by = 0; by < bh; ++by) {
      for (uint32_t bx = 0; bx < bw; ++bx) {
        uint8_t bv = bt[(size_t)by * bw + bx];
        uint32_t y1 = by * (uint32_t)bl;
        uint32_t y2 = y1 + (uint32_t)bl < h ? y1 + (uint32_t)bl : h;
        uint32_t x1 = bx * (uint32_t)bl;
        uint32_t x2 = x1 + (uint32_t)bl < w ? x1 + (uint32_t)bl : w;
        for (uint32_t yy = y1; yy < y2; ++yy) {
          const uint8_t *row = res + (size_t)yy * w;
          for (uint32_t xx = x1; xx < x2; ++xx) {
            if (!buf_u8(out, (uint8_t)(row[xx] - bv))) {
              free(bt);
              free(phase);
              free(res);
              return 0;
            }
          }
        }
      }
    }
    free(bt);
  } else if (!buf_append(out, res, pixels)) {
    free(phase);
    free(res);
    return 0;
  }
  free(phase);
  free(res);
  return 1;
}

static int try_gmodel(Candidate *best, const Image *im, int mode, int *codecs,
                      int codec_count) {
  if (mode != MODE_GRAY)
    return 1;
  size_t pixels = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels))
    return 0;
  if (pixels > 2097152u)
    return 1;
  uint8_t *g = (uint8_t *)xmalloc(pixels);
  if (!g)
    return 0;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t *p = im->rgba + i * 4u;
    g[i] = p[0];
  }
  static const int ps[] = {16, 8, 32};
  int best_p = 0;
  int best_a = 0;
  int best_b = 0;
  size_t best_matches = 0;
  for (size_t pi = 0; pi < sizeof(ps) / sizeof(ps[0]); ++pi) {
    int p = ps[pi];
    int a = gmodel_slope(g, im->width, im->height, p, 0);
    int b = gmodel_slope(g, im->width, im->height, p, 1);
    size_t matches = 0;
    if (!gmodel_matches(g, im->width, im->height, p, a, b, &matches)) {
      free(g);
      return 0;
    }
    if (matches > best_matches) {
      best_matches = matches;
      best_p = p;
      best_a = a;
      best_b = b;
    }
  }
  if (best_matches * 10u < pixels * 9u) {
    free(g);
    return 1;
  }
  Buf payload = {0};
  int okp = gmodel_payload(g, im->width, im->height, best_p, 0, best_a, best_b,
                           &payload);
  if (!okp) {
    buf_free(&payload);
    free(g);
    return 0;
  }
  if (!try_pay(best, &payload, NULL, MODE_GMODEL, TRANSFORM_IDENTITY, 0, 0,
               codecs, codec_count)) {
    buf_free(&payload);
    free(g);
    return 0;
  }
  buf_free(&payload);
  free(g);
  return 1;
}

static int fill_tile(const Image *im, uint32_t y0, uint32_t th, int channels,
                     uint8_t *tile) {
  size_t pos = 0;
  for (uint32_t y = 0; y < th; ++y) {
    const uint8_t *row = im->rgba + ((size_t)(y0 + y) * im->width) * 4u;
    for (uint32_t x = 0; x < im->width; ++x) {
      const uint8_t *p = row + (size_t)x * 4u;
      if (channels == 1) {
        tile[pos++] = p[0];
      } else if (channels == 3) {
        tile[pos++] = p[0];
        tile[pos++] = p[1];
        tile[pos++] = p[2];
      } else {
        tile[pos++] = p[0];
        tile[pos++] = p[1];
        tile[pos++] = p[2];
        tile[pos++] = p[3];
      }
    }
  }
  return 1;
}

typedef struct {
  const Image *im;
  uint32_t y0;
  uint32_t h;
  int channels;
  int search;
  unsigned inner_threads;
  uint8_t **chunk;
  size_t *size;
  int err;
} TileEncTask;

static void tile_enc_one(TileEncTask *t) {
  const uint8_t *pixels = t->im->rgba + (size_t)t->y0 * t->im->width * 4u;
  t->err = stream_encode_strided_threads(pixels, t->im->width, t->h,
                                         t->channels, 4u, t->search,
                                         t->inner_threads, t->chunk, t->size);
}

static void tile_enc_item(void *context, unsigned index) {
  tile_enc_one(&((TileEncTask *)context)[index]);
}

/* A nested native decoder needs its own byte output plus 16-bit working
   planes.  When there is more than one full band, cap simultaneous scratch at
   half of the image rows.  A single band is the unavoidable minimum for old
   streams; new routing below avoids creating near-image-sized bands.  Unused
   CPU budget remains available to the inner stream. */
static unsigned tile_band_worker_count(uint32_t height, uint32_t tile_h,
                                       uint32_t count, unsigned requested) {
  unsigned workers = requested ? requested : 1u;
  if (workers > count)
    workers = count;
  uint64_t denominator = (uint64_t)tile_h * 2u;
  uint64_t memory_workers = denominator ? (uint64_t)height / denominator : 0u;
  if (memory_workers < 1u)
    memory_workers = 1u;
  if ((uint64_t)workers > memory_workers)
    workers = (unsigned)memory_workers;
  return workers ? workers : 1u;
}

static int try_tile_height(Candidate *best, const Image *im, int channels,
                           int search, uint32_t tile_h, size_t maximum_total) {
  uint32_t count = 1u + (im->height - 1u) / tile_h;
  /* Encoder output must fit the default decoder limits. */
  if (!count || count > QLIC_DEFAULT_MAX_CHUNKS)
    return 1;
  uint8_t **chunks = (uint8_t **)calloc(count, sizeof(uint8_t *));
  size_t *sizes = (size_t *)calloc(count, sizeof(size_t));
  if (!chunks || !sizes) {
    free(chunks);
    free(sizes);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  TileEncTask *tasks = (TileEncTask *)calloc(count, sizeof(TileEncTask));
  if (!tasks) {
    free(chunks);
    free(sizes);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t y0 = i * tile_h;
    tasks[i].im = im;
    tasks[i].y0 = y0;
    tasks[i].h = im->height - y0 < tile_h ? im->height - y0 : tile_h;
    tasks[i].channels = channels;
    tasks[i].search = search;
    tasks[i].chunk = &chunks[i];
    tasks[i].size = &sizes[i];
    tasks[i].err = STREAM_E_CORRUPT;
  }
  int ok = 1;
  unsigned threads = tile_band_worker_count(im->height, tile_h, count,
                                            g_threads ? g_threads : 1u);
  unsigned budget = g_threads ? g_threads : 1u;
  Buf payload = {0};
  ok = buf_u32le(&payload, count);
  for (uint32_t i = 0; i < count && ok; ++i)
    ok = buf_u32le(&payload, 0u);
  int within_limit = ok;
  if (within_limit && (payload.size > maximum_total ||
                       QLIC_HEADER_SIZE > maximum_total - payload.size))
    within_limit = 0;
  /* Encode one worker-sized batch at a time, append it, and release its
     chunks.  This keeps at most one payload plus one batch of compressed
     chunks live instead of retaining and then duplicating the whole stream. */
  for (uint32_t base = 0; base < count && ok && within_limit; base += threads) {
    uint32_t batch = count - base < threads ? count - base : threads;
    unsigned batch_threads = batch < threads ? batch : threads;
    unsigned inner = budget / batch_threads;
    unsigned extra = budget % batch_threads;
    for (uint32_t i = 0; i < batch; ++i)
      tasks[base + i].inner_threads = inner + (i < extra);
    if (batch_threads <= 1u)
      tile_enc_one(&tasks[base]);
    else
      qlic_parallel_for(batch, batch_threads, tile_enc_item, tasks + base);
    for (uint32_t i = base; i < base + batch && ok && within_limit; ++i) {
      if (tasks[i].err != STREAM_OK) {
        set_err_status(stream_failure_status(tasks[i].err, 0),
                       "tile stream encode failed: %s",
                       stream_strerror(tasks[i].err));
        ok = 0;
        break;
      }
      if (sizes[i] > UINT32_MAX) {
        set_err("tile stream chunk is too large");
        ok = 0;
        break;
      }
      if (payload.size > maximum_total ||
          QLIC_HEADER_SIZE > maximum_total - payload.size ||
          sizes[i] > maximum_total - payload.size - QLIC_HEADER_SIZE) {
        within_limit = 0;
        break;
      }
      wr32(payload.data + 4u + (size_t)i * 4u, (uint32_t)sizes[i]);
      ok = buf_append(&payload, chunks[i], sizes[i]);
      stream_free(chunks[i]);
      chunks[i] = NULL;
    }
  }
  if (ok && within_limit)
    ok = try_store_owned(best, &payload, MODE_TILES, TRANSFORM_IDENTITY,
                         channels, tile_h);
  buf_free(&payload);
  for (uint32_t i = 0; i < count; ++i)
    stream_free(chunks[i]);
  free(tasks);
  free(chunks);
  free(sizes);
  return ok;
}

static int candidate_native_qst_info(const Candidate *candidate,
                                     int *native_mode, int *native_transform) {
  if (!candidate || candidate->mode != MODE_NATIVE ||
      candidate->codec != CODEC_STORE || !candidate->compressed ||
      candidate->compressed_size < 30u ||
      memcmp(candidate->compressed, "QST1", 4u) != 0)
    return 0;
  if (native_mode)
    *native_mode = candidate->compressed[14];
  if (native_transform)
    *native_transform = candidate->compressed[15];
  return 1;
}

/* A tiny vertical-stationarity probe predicts when resetting the native model
   in horizontal bands can repay another encode.  Sixteen evenly spaced rows
   by 32 columns are quantized to a 3-bit RGB cube.  The integer score is the
   mean total-variation distance between eight two-row bands and the global
   sample; 24576/81920 is exactly 0.30. */
static int native_tile_vertical_probe(const Image *im) {
  uint16_t global[512] = {0};
  uint8_t bands[8][512] = {{0}};
  uint32_t last_x = im->width - 1u;
  uint32_t last_y = im->height - 1u;
  for (unsigned sample_y = 0; sample_y < 16u; ++sample_y) {
    uint32_t y = (uint32_t)(((uint64_t)sample_y * last_y) / 15u);
    unsigned band = sample_y >> 1;
    const uint8_t *row = im->rgba + (size_t)y * (size_t)im->width * 4u;
    for (unsigned sample_x = 0; sample_x < 32u; ++sample_x) {
      uint32_t x = (uint32_t)(((uint64_t)sample_x * last_x) / 31u);
      const uint8_t *pixel = row + (size_t)x * 4u;
      unsigned key = ((unsigned)pixel[0] >> 5) |
                     (((unsigned)pixel[1] >> 5) << 3) |
                     (((unsigned)pixel[2] >> 5) << 6);
      ++global[key];
      ++bands[band][key];
    }
  }
  uint32_t distance = 0;
  for (unsigned band = 0; band < 8u; ++band) {
    for (unsigned color = 0; color < 512u; ++color) {
      int delta = (int)bands[band][color] * 8 - (int)global[color];
      distance += (uint32_t)(delta < 0 ? -delta : delta);
    }
  }
  return (uint64_t)distance * 10u >= 24576u;
}

static int try_native_tile_bands(Candidate *best, const Image *im, int channels,
                                 int search, size_t pixels) {
  enum { NATIVE_TILE_MIN_PIXELS = 1000000, NATIVE_TILE_MIN_SAVING = 8 * 1024 };
  int native_mode = -1;
  int native_transform = -1;
  int exploratory_route = qlic_native_tile_bands_enabled();
  int rgba_route = qlic_native_rgba_tile_bands_enabled();
  if ((!exploratory_route && !rgba_route) || pixels < NATIVE_TILE_MIN_PIXELS ||
      !candidate_native_qst_info(best, &native_mode, &native_transform))
    return 1;
  size_t incumbent = cand_size(best);
  int oracle = exploratory_route && qlic_native_tile_bands_oracle_enabled();
  int rgba_selected = rgba_route && channels == 4 && native_mode == 45 &&
                      native_transform == 2 &&
                      (uint64_t)im->height >= (uint64_t)im->width * 2u &&
                      pixels >= 3000000u;
  int needs_vertical_probe =
      rgba_selected ||
      (exploratory_route && (oracle || native_mode == 37 || native_mode == 45));
  int vertical_probe =
      needs_vertical_probe ? native_tile_vertical_probe(im) : 0;
  rgba_selected = rgba_selected && vertical_probe;
  int selected =
      rgba_selected ||
      (exploratory_route &&
       (oracle ? (((uint64_t)im->height * 2u >= (uint64_t)im->width * 3u &&
                   vertical_probe) ||
                  (native_mode == 52 && native_transform == 3))
               : (native_mode == 37 &&
                  (uint64_t)im->height * 2u >= (uint64_t)im->width * 3u &&
                  vertical_probe) ||
                     (native_mode == 45 &&
                      (uint64_t)im->height >= (uint64_t)im->width * 2u &&
                      pixels >= 3000000u && vertical_probe) ||
                     (native_mode == 52 && native_transform == 3 &&
                      im->height >= im->width &&
                      (uint64_t)incumbent * 10u >= (uint64_t)pixels * 9u)));
  if (!selected)
    return 1;
  if (incumbent < NATIVE_TILE_MIN_SAVING)
    return 1;
  size_t maximum_total = incumbent - NATIVE_TILE_MIN_SAVING;
  uint32_t tile_h =
      im->height >= 4096u && incumbent >= pixels / 8u + (pixels % 8u != 0u)
          ? 2048u
          : 256u;
  if (!try_tile_height(best, im, channels, search, tile_h, maximum_total))
    return 0;
  return 1;
}

enum {
  RTT_RAW = 0,
  RTT_FILT = 1,
  RTT_XD = 2,
  RTT_YD = 3,
  RTT_GRAD = 4,
  RTT_H2 = 5,
  RTT_V2 = 6,
  RTT_PLANAR = 7,
  RTT_RULE = 8
};

#define RTT_MAX_MODEL RTT_RULE

static int rtt_clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static int rtt_pred_causal(const uint8_t *p, uint32_t w, int ch, uint32_t x,
                           uint32_t y, int c, int model) {
  size_t row = (size_t)w * (size_t)ch;
  size_t i = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
  int left = x ? p[i - (size_t)ch] : 0;
  int up = y ? p[i - row] : 0;
  int up_left = (x && y) ? p[i - row - (size_t)ch] : 0;
  int left2 = x > 1 ? p[i - (size_t)ch * 2u] : left;
  int up2 = y > 1 ? p[i - row * 2u] : up;
  switch (model) {
  case RTT_XD:
    return left;
  case RTT_YD:
    return up;
  case RTT_H2:
    return x ? rtt_clamp8(2 * left - left2) : 0;
  case RTT_V2:
    return y ? rtt_clamp8(2 * up - up2) : 0;
  default:
    return pred(5, left, up, up_left);
  }
}

static int rtt_pred_planar(const uint8_t *par, uint32_t w, uint32_t h,
                           uint32_t x, uint32_t y, int c) {
  int base = par[(size_t)c * 3u + 0u];
  int right = par[(size_t)c * 3u + 1u];
  int bottom = par[(size_t)c * 3u + 2u];
  int64_t px =
      w > 1 ? ((int64_t)(right - base) * (int64_t)x) / (int64_t)(w - 1u) : 0;
  int64_t py =
      h > 1 ? ((int64_t)(bottom - base) * (int64_t)y) / (int64_t)(h - 1u) : 0;
  return (int)((base + px + py) & 255);
}

static int rtt_planar_params(const uint8_t *tile, uint32_t w, uint32_t h,
                             int ch, uint8_t *par) {
  if (!w || !h)
    return 0;
  for (int c = 0; c < ch; ++c) {
    par[(size_t)c * 3u + 0u] = tile[c];
    par[(size_t)c * 3u + 1u] =
        tile[((size_t)(w - 1u) * (size_t)ch) + (size_t)c];
    par[(size_t)c * 3u + 2u] =
        tile[((size_t)(h - 1u) * (size_t)w * (size_t)ch) + (size_t)c];
  }
  return 1;
}

static int rtt_abs(int v) { return v < 0 ? -v : v; }

static int rtt_rule_energy(int left, int up, int up_left, int left2, int up2,
                           int has_left, int has_up) {
  int e;
  if (has_left && has_up)
    e = rtt_abs(left - up) + rtt_abs(left - up_left) + rtt_abs(up - up_left);
  else if (has_left)
    e = rtt_abs(left - left2);
  else if (has_up)
    e = rtt_abs(up - up2);
  else
    e = 0;
  return e > 255 ? 255 : e;
}

static int rtt_rule_branch(int left, int up, int up_left, int left2, int up2,
                           int has_left, int has_up, const uint8_t *rule) {
  int e = rtt_rule_energy(left, up, up_left, left2, up2, has_left, has_up);
  if (e <= (int)rule[0])
    return 0;
  int sx = has_left ? rtt_abs(left - left2) : 512;
  int sy = has_up ? rtt_abs(up - up2) : 512;
  int gap = (int)rule[3];
  if (sx + gap < sy)
    return 1;
  if (sy + gap < sx)
    return 2;
  return e >= (int)rule[1] ? 3 : 0;
}

static int rtt_rule_pred(int id, int left, int up, int up_left, int has_left,
                         int has_up) {
  switch (id) {
  case 0:
    return pred(5, left, up, up_left);
  case 1:
    return pred(4, left, up, up_left);
  case 2:
    return has_left ? left : (has_up ? up : 0);
  default:
    return has_up ? up : (has_left ? left : 0);
  }
}

static int rtt_rule_predict(const uint8_t *p, uint32_t w, int ch, uint32_t x,
                            uint32_t y, int c, const uint8_t *rule) {
  size_t row = (size_t)w * (size_t)ch;
  size_t i = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
  int has_left = x != 0;
  int has_up = y != 0;
  int left = has_left ? p[i - (size_t)ch] : 0;
  int up = has_up ? p[i - row] : 0;
  int up_left = (has_left && has_up) ? p[i - row - (size_t)ch] : 0;
  int left2 = x > 1u ? p[i - (size_t)ch * 2u] : left;
  int up2 = y > 1u ? p[i - row * 2u] : up;
  int branch =
      rtt_rule_branch(left, up, up_left, left2, up2, has_left, has_up, rule);
  int pid = (rule[2] >> (branch * 2)) & 3;
  return rtt_rule_pred(pid, left, up, up_left, has_left, has_up);
}

static void rtt_rule_quantiles(const uint8_t *tile, uint32_t w, uint32_t h,
                               int ch, uint8_t *lo, uint8_t *hi) {
  uint32_t hist[256];
  memset(hist, 0, sizeof(hist));
  uint64_t total = 0;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      for (int c = 0; c < ch; ++c) {
        size_t row = (size_t)w * (size_t)ch;
        size_t i = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
        int has_left = x != 0;
        int has_up = y != 0;
        int left = has_left ? tile[i - (size_t)ch] : 0;
        int up = has_up ? tile[i - row] : 0;
        int up_left = (has_left && has_up) ? tile[i - row - (size_t)ch] : 0;
        int left2 = x > 1u ? tile[i - (size_t)ch * 2u] : left;
        int up2 = y > 1u ? tile[i - row * 2u] : up;
        int e =
            rtt_rule_energy(left, up, up_left, left2, up2, has_left, has_up);
        ++hist[e];
        ++total;
      }
    }
  }
  uint64_t q1 = total / 4u;
  uint64_t q3 = (total * 3u) / 4u;
  uint64_t sum = 0;
  uint8_t a = 16;
  uint8_t b = 96;
  for (int i = 0; i < 256; ++i) {
    sum += hist[i];
    if (sum >= q1) {
      a = (uint8_t)i;
      break;
    }
  }
  sum = 0;
  for (int i = 0; i < 256; ++i) {
    sum += hist[i];
    if (sum >= q3) {
      b = (uint8_t)i;
      break;
    }
  }
  if (b <= a) {
    int v = (int)a + 16;
    if (v > 255)
      v = 255;
    b = (uint8_t)v;
    if (b <= a && a > 0)
      a = (uint8_t)(a - 1u);
  }
  *lo = a;
  *hi = b;
}

typedef struct {
  uint8_t lo;
  uint8_t hi;
  uint8_t gap;
} RTTRuleSeed;

static uint64_t rtt_rule_fit(const uint8_t *tile, uint32_t w, uint32_t h,
                             int ch, const RTTRuleSeed *seed, uint8_t *map) {
  uint64_t score[16];
  memset(score, 0, sizeof(score));
  uint8_t rule[4] = {seed->lo, seed->hi, 0, seed->gap};
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      for (int c = 0; c < ch; ++c) {
        size_t row = (size_t)w * (size_t)ch;
        size_t i = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
        int has_left = x != 0;
        int has_up = y != 0;
        int left = has_left ? tile[i - (size_t)ch] : 0;
        int up = has_up ? tile[i - row] : 0;
        int up_left = (has_left && has_up) ? tile[i - row - (size_t)ch] : 0;
        int left2 = x > 1u ? tile[i - (size_t)ch * 2u] : left;
        int up2 = y > 1u ? tile[i - row * 2u] : up;
        int branch = rtt_rule_branch(left, up, up_left, left2, up2, has_left,
                                     has_up, rule);
        for (int p = 0; p < 4; ++p) {
          int pr = rtt_rule_pred(p, left, up, up_left, has_left, has_up);
          int d = ((int)tile[i] - pr) & 255;
          score[branch * 4 + p] += (uint64_t)(d <= 128 ? d : 256 - d);
        }
      }
    }
  }
  uint64_t total = 0;
  uint8_t m = 0;
  for (int b = 0; b < 4; ++b) {
    uint64_t best = UINT64_MAX;
    int pid = 0;
    for (int p = 0; p < 4; ++p) {
      uint64_t s = score[b * 4 + p];
      if (s < best) {
        best = s;
        pid = p;
      }
    }
    total += best;
    m = (uint8_t)(m | (uint8_t)(pid << (b * 2)));
  }
  *map = m;
  return total;
}

static int rtt_rule_params(const uint8_t *tile, uint32_t w, uint32_t h, int ch,
                           uint8_t *rule) {
  uint8_t qlo = 0;
  uint8_t qhi = 0;
  rtt_rule_quantiles(tile, w, h, ch, &qlo, &qhi);
  RTTRuleSeed seeds[8] = {{8, 48, 0},    {16, 64, 4}, {24, 96, 8},
                          {32, 128, 16}, {8, 96, 16}, {48, 160, 16},
                          {0, 0, 8},     {0, 0, 16}};
  int n = 8;
  seeds[6].lo = qlo;
  seeds[6].hi = qhi;
  seeds[7].lo = (uint8_t)(qlo >> 1);
  seeds[7].hi = qhi;
  uint64_t best = UINT64_MAX;
  uint8_t best_rule[4] = {16, 64, 0, 4};
  for (int i = 0; i < n; ++i) {
    if (seeds[i].hi <= seeds[i].lo)
      continue;
    uint8_t map = 0;
    uint64_t s = rtt_rule_fit(tile, w, h, ch, &seeds[i], &map);
    if (s < best) {
      best = s;
      best_rule[0] = seeds[i].lo;
      best_rule[1] = seeds[i].hi;
      best_rule[2] = map;
      best_rule[3] = seeds[i].gap;
    }
  }
  memcpy(rule, best_rule, 4);
  return 1;
}

static int rtt_make_residual(const uint8_t *tile, uint32_t w, uint32_t h,
                             int ch, int model, const uint8_t *par, Buf *res) {
  size_t pixels = 0, bytes = 0;
  if (!mulok((size_t)w, (size_t)h, &pixels) ||
      !mulok(pixels, (size_t)ch, &bytes))
    return 0;
  res->data = (uint8_t *)xmalloc(bytes);
  if (!res->data)
    return 0;
  res->size = bytes;
  res->cap = bytes;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      for (int c = 0; c < ch; ++c) {
        size_t i = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
        int pr = model == RTT_PLANAR
                     ? rtt_pred_planar(par, w, h, x, y, c)
                     : (model == RTT_RULE
                            ? rtt_rule_predict(tile, w, ch, x, y, c, par)
                            : rtt_pred_causal(tile, w, ch, x, y, c, model));
        res->data[i] = (uint8_t)(tile[i] - pr);
      }
    }
  }
  return 1;
}

static int rtt_encode_model(const uint8_t *tile, uint32_t w, uint32_t h, int ch,
                            int model, int stream_search, unsigned threads,
                            uint8_t **out, size_t *outn) {
  *out = NULL;
  *outn = 0;
  if (model == RTT_RAW) {
    int err = stream_encode_threads(tile, w, h, ch, stream_search, threads, out,
                                    outn);
    if (err == STREAM_E_DIM)
      return 1;
    if (err != STREAM_OK) {
      set_err_status(stream_failure_status(err, 0),
                     "tile model raw encode failed: %s", stream_strerror(err));
      return 0;
    }
    return 1;
  }
  if (model == RTT_FILT) {
    size_t row_bytes = 0;
    if (!mulok((size_t)w, (size_t)ch, &row_bytes))
      return 0;
    if (row_bytes + 1u > UINT32_MAX) {
      set_err("tile model filtered row is too wide");
      return 0;
    }
    Buf filtered = {0};
    if (!mk_frows(tile, row_bytes, h, ch, stream_search, &filtered))
      return 0;
    int err = stream_encode_threads(filtered.data, (uint32_t)(row_bytes + 1u),
                                    h, 1, stream_search, threads, out, outn);
    buf_free(&filtered);
    if (err == STREAM_E_DIM)
      return 1;
    if (err != STREAM_OK) {
      set_err_status(stream_failure_status(err, 0),
                     "tile model filter encode failed: %s",
                     stream_strerror(err));
      return 0;
    }
    return 1;
  }

  uint8_t par[12] = {0};
  size_t parn = 0;
  if (model == RTT_PLANAR) {
    if (!rtt_planar_params(tile, w, h, ch, par))
      return 0;
    parn = (size_t)ch * 3u;
  } else if (model == RTT_RULE) {
    if (!rtt_rule_params(tile, w, h, ch, par))
      return 0;
    parn = 4u;
  }
  Buf res = {0};
  if (!rtt_make_residual(tile, w, h, ch, model, par, &res)) {
    buf_free(&res);
    return 0;
  }
  uint8_t *z = NULL;
  size_t zn = 0;
  int err = stream_encode_threads(res.data, w, h, ch, stream_search, threads,
                                  &z, &zn);
  buf_free(&res);
  if (err == STREAM_E_DIM)
    return 1;
  if (err != STREAM_OK) {
    set_err_status(stream_failure_status(err, 0),
                   "tile model residual encode failed: %s",
                   stream_strerror(err));
    return 0;
  }
  if (parn) {
    if (zn > SIZE_MAX - parn) {
      stream_free(z);
      set_err("size overflow");
      return 0;
    }
    uint8_t *p = (uint8_t *)xmalloc(parn + zn);
    if (!p) {
      stream_free(z);
      return 0;
    }
    memcpy(p, par, parn);
    memcpy(p + parn, z, zn);
    stream_free(z);
    *out = p;
    *outn = parn + zn;
  } else {
    *out = z;
    *outn = zn;
  }
  return 1;
}

typedef struct {
  const Image *im;
  uint32_t y0;
  uint32_t h;
  int channels;
  int stream_search;
  unsigned inner_threads;
  uint8_t models[RTT_MAX_MODEL + 1];
  int model_count;
  uint8_t *chunk;
  size_t size;
  uint8_t chosen;
  int ok;
} RTTEncTask;

static void rtt_enc_one(RTTEncTask *t) {
  size_t tile_bytes = 0;
  if (!mulok((size_t)t->im->width, (size_t)t->h, &tile_bytes) ||
      !mulok(tile_bytes, (size_t)t->channels, &tile_bytes)) {
    t->ok = 0;
    return;
  }
  uint8_t *tile = (uint8_t *)xmalloc(tile_bytes);
  if (!tile) {
    t->ok = 0;
    return;
  }
  fill_tile(t->im, t->y0, t->h, t->channels, tile);
  uint8_t *best_data = NULL;
  size_t best_size = SIZE_MAX;
  uint8_t best_model = RTT_RAW;
  for (int m = 0; m < t->model_count; ++m) {
    uint8_t *data = NULL;
    size_t size = 0;
    if (!rtt_encode_model(tile, t->im->width, t->h, t->channels, t->models[m],
                          t->stream_search, t->inner_threads, &data, &size)) {
      free(best_data);
      free(tile);
      t->ok = 0;
      return;
    }
    if (data && size < best_size) {
      free(best_data);
      best_data = data;
      best_size = size;
      best_model = t->models[m];
    } else {
      free(data);
    }
  }
  free(tile);
  if (!best_data) {
    t->ok = 0;
    return;
  }
  t->chunk = best_data;
  t->size = best_size;
  t->chosen = best_model;
  t->ok = 1;
}

static void rtt_enc_item(void *context, unsigned index) {
  rtt_enc_one(&((RTTEncTask *)context)[index]);
}

static int run_rtt_enc(RTTEncTask *tasks, uint32_t count, unsigned threads) {
  if (threads < 1u)
    threads = 1u;
  if (threads > count)
    threads = count;
  if (threads <= 1u) {
    for (uint32_t i = 0; i < count; ++i)
      rtt_enc_one(&tasks[i]);
    return 1;
  }
  qlic_parallel_for(count, threads, rtt_enc_item, tasks);
  return 1;
}

static int try_rtt_height(Candidate *best, const Image *im, int channels,
                          int search, int stream_search, uint32_t tile_h) {
  uint32_t count = 1u + (im->height - 1u) / tile_h;
  if (!count || count > 65536u)
    return 1;
  RTTEncTask *tasks = (RTTEncTask *)calloc(count, sizeof(RTTEncTask));
  if (!tasks) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  uint8_t models[RTT_MAX_MODEL + 1] = {RTT_RAW, RTT_GRAD, RTT_XD,
                                       RTT_YD,  RTT_RULE, RTT_FILT,
                                       RTT_H2,  RTT_V2,   RTT_PLANAR};
  int model_count = search >= 7 ? RTT_MAX_MODEL + 1 : 2;
  unsigned threads = g_threads ? g_threads : 1u;
  if (threads > count)
    threads = count;
  unsigned budget = g_threads ? g_threads : 1u;
  unsigned inner = budget / threads;
  unsigned extra = budget % threads;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t y0 = i * tile_h;
    tasks[i].im = im;
    tasks[i].y0 = y0;
    tasks[i].h = im->height - y0 < tile_h ? im->height - y0 : tile_h;
    tasks[i].channels = channels;
    tasks[i].stream_search = stream_search;
    tasks[i].inner_threads = inner + (i < extra);
    tasks[i].model_count = model_count;
    memcpy(tasks[i].models, models, (size_t)model_count);
  }
  int ok = run_rtt_enc(tasks, count, threads);
  for (uint32_t i = 0; i < count && ok; ++i) {
    if (!tasks[i].ok) {
      set_err("tile model could not encode tile");
      ok = 0;
    }
  }
  if (ok) {
    Buf payload = {0};
    ok = buf_u32le(&payload, count);
    for (uint32_t i = 0; i < count && ok; ++i) {
      if (tasks[i].size > UINT32_MAX) {
        set_err("tile model chunk is too large");
        ok = 0;
        break;
      }
      ok = buf_u8(&payload, tasks[i].chosen) &&
           buf_u32le(&payload, (uint32_t)tasks[i].size);
    }
    for (uint32_t i = 0; i < count && ok; ++i)
      ok = buf_append(&payload, tasks[i].chunk, tasks[i].size);
    if (ok) {
      ok = try_store_owned(best, &payload, MODE_TILE_MODEL, TRANSFORM_IDENTITY,
                           channels, tile_h);
    }
    buf_free(&payload);
  }
  for (uint32_t i = 0; i < count; ++i)
    free(tasks[i].chunk);
  free(tasks);
  return ok;
}

#define BLK_SIZE 16u
#define BLK_RAW 0u
#define BLK_FLAT 1u
#define BLK_LEFT 2u
#define BLK_UP 3u
#define BLK_TWO 4u
#define BLK_FOUR 5u
#define BLK_PAT2 6u
#define BLK_REF 7u
#define BLK2_ZERO 0u
#define BLK2_FLAT 1u
#define BLK2_GRAD 2u
#define BLK2_LEFT 3u
#define BLK2_UP 4u
#define BLK2_CAUSAL 5u
#define BLK2_PDM 6u
#define BLK2_MAX_PAR 64u
#define PDM_SIZE 64u
#define CF_SIZE 64u

typedef struct {
  uint32_t v[4];
} BlockPattern;

typedef struct {
  uint64_t key;
  size_t index_plus_one;
} BlockRefEntry;

typedef struct {
  BlockRefEntry *entries;
  size_t cap;
} BlockRefTable;

static int blk_refs_init(BlockRefTable *table, size_t blocks);
static size_t blk_refs_find(BlockRefTable *table, const Image *im, uint32_t x,
                            uint32_t y, uint32_t bw, uint32_t bh, int ch,
                            size_t index, uint32_t blocks_x);

static inline uint32_t block_extent(uint32_t total, uint32_t origin,
                                    uint32_t limit) {
  uint32_t left = total - origin;
  return left < limit ? left : limit;
}

static uint32_t blk_color(const Image *im, uint32_t x, uint32_t y, int ch) {
  const uint8_t *p = im->rgba + ((size_t)y * im->width + x) * 4u;
  if (ch == 1)
    return p[0];
  if (ch == 3)
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void blk_set(Image *im, uint32_t x, uint32_t y, uint32_t c, int ch) {
  uint8_t *p = im->rgba + ((size_t)y * im->width + x) * 4u;
  p[0] = (uint8_t)c;
  p[1] = ch == 1 ? (uint8_t)c : (uint8_t)(c >> 8);
  p[2] = ch == 1 ? (uint8_t)c : (uint8_t)(c >> 16);
  p[3] = ch == 4 ? (uint8_t)(c >> 24) : 255;
}

static int blk_put_color(Buf *b, uint32_t v, int ch) {
  uint8_t c[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  return buf_append(b, c, (size_t)ch);
}

static int blk_same(const Image *im, uint32_t x, uint32_t y, uint32_t sx,
                    uint32_t sy, uint32_t bw, uint32_t bh, int ch) {
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      if (blk_color(im, x + xx, y + yy, ch) !=
          blk_color(im, sx + xx, sy + yy, ch))
        return 0;
    }
  }
  return 1;
}

static int blk_flat(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                    uint32_t bh, int ch, uint32_t *color) {
  uint32_t c = blk_color(im, x, y, ch);
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      if (blk_color(im, x + xx, y + yy, ch) != c)
        return 0;
    }
  }
  *color = c;
  return 1;
}

static int blk_colors(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                      uint32_t bh, uint32_t *colors, uint8_t *idx, int maxc,
                      int ch, int *count) {
  int n = 0;
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      uint32_t c = blk_color(im, x + xx, y + yy, ch);
      int k = 0;
      while (k < n && colors[k] != c)
        ++k;
      if (k == n) {
        if (n == maxc)
          return 0;
        colors[n++] = c;
      }
      idx[(size_t)yy * bw + xx] = (uint8_t)k;
    }
  }
  *count = n;
  return 1;
}

static int blk_pat_eq(const BlockPattern *a, const BlockPattern *b) {
  return a->v[0] == b->v[0] && a->v[1] == b->v[1] && a->v[2] == b->v[2] &&
         a->v[3] == b->v[3];
}

static void blk_get_pat(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                        uint32_t bh, uint32_t px, uint32_t py, int ch,
                        BlockPattern *p) {
  uint32_t x0 = x + px * 2u;
  uint32_t y0 = y + py * 2u;
  uint32_t x1 = px * 2u + 1u < bw ? x0 + 1u : x0;
  uint32_t y1 = py * 2u + 1u < bh ? y0 + 1u : y0;
  p->v[0] = blk_color(im, x0, y0, ch);
  p->v[1] = blk_color(im, x1, y0, ch);
  p->v[2] = blk_color(im, x0, y1, ch);
  p->v[3] = blk_color(im, x1, y1, ch);
}

static int blk_patterns(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                        uint32_t bh, BlockPattern *pat, uint8_t *idx, int maxp,
                        int ch, int *count) {
  uint32_t pw = (bw + 1u) >> 1;
  uint32_t ph = (bh + 1u) >> 1;
  int n = 0;
  for (uint32_t py = 0; py < ph; ++py) {
    for (uint32_t px = 0; px < pw; ++px) {
      BlockPattern p;
      blk_get_pat(im, x, y, bw, bh, px, py, ch, &p);
      int k = 0;
      while (k < n && !blk_pat_eq(&pat[k], &p))
        ++k;
      if (k == n) {
        if (n == maxp)
          return 0;
        pat[n++] = p;
      }
      idx[(size_t)py * pw + px] = (uint8_t)k;
    }
  }
  *count = n;
  return 1;
}

static int blk_bits(int n) {
  int b = 0;
  int v = n - 1;
  while (v > 0) {
    ++b;
    v >>= 1;
  }
  return b;
}

static int blk_pack_bits(Buf *b, const uint8_t *idx, size_t n, int bits) {
  if (bits <= 0)
    return 1;
  uint32_t acc = 0;
  int used = 0;
  for (size_t i = 0; i < n; ++i) {
    acc |= (uint32_t)idx[i] << used;
    used += bits;
    while (used >= 8) {
      if (!buf_u8(b, (uint8_t)acc))
        return 0;
      acc >>= 8;
      used -= 8;
    }
  }
  if (used && !buf_u8(b, (uint8_t)acc))
    return 0;
  return 1;
}

static int blk_write_raw(Buf *b, const Image *im, uint32_t x, uint32_t y,
                         uint32_t bw, uint32_t bh, int ch) {
  if (!buf_u8(b, BLK_RAW))
    return 0;
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      uint32_t c = blk_color(im, x + xx, y + yy, ch);
      if (!blk_put_color(b, c, ch))
        return 0;
    }
  }
  return 1;
}

static int blk_write_two(Buf *b, uint32_t *colors, const uint8_t *idx,
                         uint32_t area, int ch) {
  return buf_u8(b, BLK_TWO) && blk_put_color(b, colors[0], ch) &&
         blk_put_color(b, colors[1], ch) && blk_pack_bits(b, idx, area, 1);
}

static int blk_write_four(Buf *b, uint32_t *colors, int count,
                          const uint8_t *idx, uint32_t area, int ch) {
  if (!buf_u8(b, BLK_FOUR) || !buf_u8(b, (uint8_t)count))
    return 0;
  for (int i = 0; i < count; ++i) {
    if (!blk_put_color(b, colors[i], ch))
      return 0;
  }
  return blk_pack_bits(b, idx, area, blk_bits(count));
}

static size_t blk_varint_size(size_t v) {
  size_t n = 1u;
  while (v >= 128u) {
    v >>= 7;
    ++n;
  }
  return n;
}

static int blk_write_pat(Buf *b, const BlockPattern *pat, int count,
                         const uint8_t *idx, uint32_t subn, int ch) {
  if (!buf_u8(b, BLK_PAT2) || !buf_u8(b, (uint8_t)count))
    return 0;
  for (int i = 0; i < count; ++i) {
    for (int j = 0; j < 4; ++j) {
      if (!blk_put_color(b, pat[i].v[j], ch))
        return 0;
    }
  }
  return blk_pack_bits(b, idx, subn, blk_bits(count));
}

static int blk_write_one(Buf *b, const Image *im, uint32_t x, uint32_t y,
                         int ch, size_t ref_distance, int *extended) {
  uint32_t bw = block_extent(im->width, x, BLK_SIZE);
  uint32_t bh = block_extent(im->height, y, BLK_SIZE);
  uint32_t area = bw * bh;
  uint32_t flat = 0;
  uint32_t colors[4];
  uint8_t idx[BLK_SIZE * BLK_SIZE];
  BlockPattern pat[16];
  uint8_t pidx[64];
  int cc = 0;
  int pc = 0;
  uint32_t pw = (bw + 1u) >> 1;
  uint32_t ph = (bh + 1u) >> 1;
  uint32_t subn = pw * ph;
  size_t best = 1u + (size_t)area * (size_t)ch;
  int op = BLK_RAW;
  if (blk_flat(im, x, y, bw, bh, ch, &flat)) {
    best = 1u + (size_t)ch;
    op = BLK_FLAT;
  }
  if (x >= BLK_SIZE && blk_same(im, x, y, x - BLK_SIZE, y, bw, bh, ch)) {
    best = 1u;
    op = BLK_LEFT;
  }
  if (y >= BLK_SIZE && blk_same(im, x, y, x, y - BLK_SIZE, bw, bh, ch)) {
    best = 1u;
    op = BLK_UP;
  }
  int have_colors = blk_colors(im, x, y, bw, bh, colors, idx, 4, ch, &cc);
  if (have_colors && cc == 2) {
    size_t cost = 1u + (size_t)cc * (size_t)ch + ((size_t)area + 7u) / 8u;
    if (cost < best) {
      best = cost;
      op = BLK_TWO;
    }
  }
  if (have_colors && cc > 2 && cc <= 4) {
    size_t cost = 2u + (size_t)cc * (size_t)ch +
                  (((size_t)area * (size_t)blk_bits(cc)) + 7u) / 8u;
    if (cost < best) {
      best = cost;
      op = BLK_FOUR;
    }
  }
  if (blk_patterns(im, x, y, bw, bh, pat, pidx, 16, ch, &pc) && pc > 1) {
    size_t cost = 2u + (size_t)pc * 4u * (size_t)ch +
                  (((size_t)subn * (size_t)blk_bits(pc)) + 7u) / 8u;
    if (cost < best)
      op = BLK_PAT2;
  }
  if (ref_distance) {
    size_t cost = 1u + blk_varint_size(ref_distance);
    if (cost < best)
      op = BLK_REF;
  }
  switch (op) {
  case BLK_FLAT:
    return buf_u8(b, BLK_FLAT) && blk_put_color(b, flat, ch);
  case BLK_LEFT:
    return buf_u8(b, BLK_LEFT);
  case BLK_UP:
    return buf_u8(b, BLK_UP);
  case BLK_TWO:
    return blk_write_two(b, colors, idx, area, ch);
  case BLK_FOUR:
    return blk_write_four(b, colors, cc, idx, area, ch);
  case BLK_PAT2:
    return blk_write_pat(b, pat, pc, pidx, subn, ch);
  case BLK_REF:
    *extended = 1;
    return buf_u8(b, BLK_REF) && buf_varint(b, ref_distance);
  default:
    return blk_write_raw(b, im, x, y, bw, bh, ch);
  }
}

static uint8_t u8_sat(int v) {
  if (v < 0)
    return 0;
  if (v > 255)
    return 255;
  return (uint8_t)v;
}

static int s8_val(uint8_t v) { return v < 128u ? (int)v : (int)v - 256; }

static uint8_t fold_delta(int d) {
  if (d > 127)
    d -= 256;
  else if (d < -128)
    d += 256;
  return d >= 0 ? (uint8_t)(d * 2) : (uint8_t)((-d) * 2 - 1);
}

static int unfold_delta(uint8_t v) {
  return (v & 1u) ? -((int)v + 1) / 2 : (int)v / 2;
}

static size_t blk2_i(uint32_t w, int ch, uint32_t x, uint32_t y, int c) {
  return ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
}

static uint8_t blk2_sample(const Image *im, uint32_t x, uint32_t y, int ch,
                           int c) {
  const uint8_t *p = im->rgba + ((size_t)y * im->width + x) * 4u;
  return ch == 1 ? p[0] : p[c];
}

static uint8_t blk2_sample_buf(const uint8_t *pix, uint32_t w, int ch,
                               uint32_t x, uint32_t y, int c) {
  return pix[blk2_i(w, ch, x, y, c)];
}

static size_t blk2_parn(uint8_t op, int ch) {
  if (op == BLK2_FLAT || op == BLK2_LEFT || op == BLK2_UP)
    return (size_t)ch;
  if (op == BLK2_GRAD)
    return (size_t)ch * 4u;
  if (op == BLK2_PDM)
    return 3u + (size_t)ch * 7u;
  return 0;
}

static int blk2_paeth(int a, int b, int c) {
  int p = a + b - c;
  int pa = abs(p - a);
  int pb = abs(p - b);
  int pc = abs(p - c);
  if (pa <= pb && pa <= pc)
    return a;
  return pb <= pc ? b : c;
}

static uint8_t blk2_causal_img(const Image *im, uint32_t x, uint32_t y, int ch,
                               int c) {
  if (!x && !y)
    return 0;
  int w =
      x ? blk2_sample(im, x - 1u, y, ch, c) : blk2_sample(im, x, y - 1u, ch, c);
  int n = y ? blk2_sample(im, x, y - 1u, ch, c) : w;
  int nw = (x && y) ? blk2_sample(im, x - 1u, y - 1u, ch, c) : n;
  return (uint8_t)blk2_paeth(w, n, nw);
}

static uint8_t blk2_causal_buf(const uint8_t *pix, uint32_t width, int ch,
                               uint32_t x, uint32_t y, int c) {
  if (!x && !y)
    return 0;
  int w = x ? blk2_sample_buf(pix, width, ch, x - 1u, y, c)
            : blk2_sample_buf(pix, width, ch, x, y - 1u, c);
  int n = y ? blk2_sample_buf(pix, width, ch, x, y - 1u, c) : w;
  int nw = (x && y) ? blk2_sample_buf(pix, width, ch, x - 1u, y - 1u, c) : n;
  return (uint8_t)blk2_paeth(w, n, nw);
}

static uint8_t blk2_linear_img(const Image *im, uint32_t x, uint32_t y, int ch,
                               int c) {
  if (!x || !y)
    return blk2_causal_img(im, x, y, ch, c);
  int w = blk2_sample(im, x - 1u, y, ch, c);
  int n = blk2_sample(im, x, y - 1u, ch, c);
  int nw = blk2_sample(im, x - 1u, y - 1u, ch, c);
  return u8_sat(w + n - nw);
}

static uint8_t blk2_linear_buf(const uint8_t *pix, uint32_t width, int ch,
                               uint32_t x, uint32_t y, int c) {
  if (!x || !y)
    return blk2_causal_buf(pix, width, ch, x, y, c);
  int w = blk2_sample_buf(pix, width, ch, x - 1u, y, c);
  int n = blk2_sample_buf(pix, width, ch, x, y - 1u, c);
  int nw = blk2_sample_buf(pix, width, ch, x - 1u, y - 1u, c);
  return u8_sat(w + n - nw);
}

static uint8_t blk2_grad(const uint8_t *p, uint32_t bw, uint32_t bh,
                         uint32_t xx, uint32_t yy, int c) {
  const uint8_t *q = p + (size_t)c * 4u;
  if (bw == 1u && bh == 1u)
    return q[0];
  if (bh == 1u) {
    uint32_t dx = bw - 1u;
    return (
        uint8_t)(((uint32_t)q[0] * (dx - xx) + (uint32_t)q[1] * xx + dx / 2u) /
                 dx);
  }
  if (bw == 1u) {
    uint32_t dy = bh - 1u;
    return (
        uint8_t)(((uint32_t)q[0] * (dy - yy) + (uint32_t)q[2] * yy + dy / 2u) /
                 dy);
  }
  uint32_t dx = bw - 1u;
  uint32_t dy = bh - 1u;
  uint32_t top =
      ((uint32_t)q[0] * (dx - xx) + (uint32_t)q[1] * xx + dx / 2u) / dx;
  uint32_t bot =
      ((uint32_t)q[2] * (dx - xx) + (uint32_t)q[3] * xx + dx / 2u) / dx;
  return (uint8_t)((top * (dy - yy) + bot * yy + dy / 2u) / dy);
}

static uint8_t pdm_q(const uint8_t *par, int i) {
  if (i == 0)
    return par[0] & 15u;
  if (i == 1)
    return par[0] >> 4;
  if (i == 2)
    return par[1] & 15u;
  if (i == 3)
    return par[1] >> 4;
  return par[2] & 15u;
}

static uint8_t pdm_lerp(uint8_t a, uint8_t b, uint8_t q) {
  return (uint8_t)(((uint32_t)a * (15u - q) + (uint32_t)b * q + 7u) / 15u);
}

static void pdm_pack(uint8_t *par, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                     uint8_t e) {
  par[0] = (uint8_t)((a & 15u) | ((b & 15u) << 4));
  par[1] = (uint8_t)((c & 15u) | ((d & 15u) << 4));
  par[2] = (uint8_t)(e & 15u);
}

static uint8_t pdm_pred_img(const Image *im, uint32_t x, uint32_t y,
                            uint32_t bw, uint32_t bh, uint32_t step,
                            uint32_t xx, uint32_t yy, int ch, int c,
                            const uint8_t *par) {
  const uint8_t *flat = par + 3u;
  const uint8_t *grad = flat + (size_t)ch;
  const uint8_t *left = grad + (size_t)ch * 4u;
  const uint8_t *up = left + (size_t)ch;
  uint8_t ca = blk2_causal_img(im, x + xx, y + yy, ch, c);
  uint8_t ln = blk2_linear_img(im, x + xx, y + yy, ch, c);
  uint8_t lp = ca;
  uint8_t upv = ca;
  if (x >= step) {
    uint8_t v = blk2_sample(im, x - step + xx, y + yy, ch, c);
    lp = u8_sat((int)v + s8_val(left[c]));
  }
  if (y >= step) {
    uint8_t v = blk2_sample(im, x + xx, y - step + yy, ch, c);
    upv = u8_sat((int)v + s8_val(up[c]));
  }
  uint8_t axis = pdm_lerp(upv, lp, pdm_q(par, 1));
  uint8_t edge = pdm_lerp(ca, ln, pdm_q(par, 4));
  uint8_t nb = pdm_lerp(axis, edge, pdm_q(par, 3));
  uint8_t block =
      pdm_lerp(flat[c], blk2_grad(grad, bw, bh, xx, yy, c), pdm_q(par, 2));
  return pdm_lerp(nb, block, pdm_q(par, 0));
}

static uint8_t pdm_pred_buf(const uint8_t *pix, uint32_t w, uint32_t x,
                            uint32_t y, uint32_t bw, uint32_t bh, uint32_t step,
                            uint32_t xx, uint32_t yy, int ch, int c,
                            const uint8_t *par) {
  const uint8_t *flat = par + 3u;
  const uint8_t *grad = flat + (size_t)ch;
  const uint8_t *left = grad + (size_t)ch * 4u;
  const uint8_t *up = left + (size_t)ch;
  uint8_t ca = blk2_causal_buf(pix, w, ch, x + xx, y + yy, c);
  uint8_t ln = blk2_linear_buf(pix, w, ch, x + xx, y + yy, c);
  uint8_t lp = ca;
  uint8_t upv = ca;
  if (x >= step) {
    uint8_t v = pix[blk2_i(w, ch, x - step + xx, y + yy, c)];
    lp = u8_sat((int)v + s8_val(left[c]));
  }
  if (y >= step) {
    uint8_t v = pix[blk2_i(w, ch, x + xx, y - step + yy, c)];
    upv = u8_sat((int)v + s8_val(up[c]));
  }
  uint8_t axis = pdm_lerp(upv, lp, pdm_q(par, 1));
  uint8_t edge = pdm_lerp(ca, ln, pdm_q(par, 4));
  uint8_t nb = pdm_lerp(axis, edge, pdm_q(par, 3));
  uint8_t block =
      pdm_lerp(flat[c], blk2_grad(grad, bw, bh, xx, yy, c), pdm_q(par, 2));
  return pdm_lerp(nb, block, pdm_q(par, 0));
}

static uint8_t blk2_pred_img(const Image *im, uint32_t x, uint32_t y,
                             uint32_t bw, uint32_t bh, uint32_t xx, uint32_t yy,
                             int ch, int c, uint8_t op, const uint8_t *par) {
  if (op == BLK2_FLAT)
    return par[c];
  if (op == BLK2_GRAD)
    return blk2_grad(par, bw, bh, xx, yy, c);
  if (op == BLK2_LEFT)
    return u8_sat((int)blk2_sample(im, x - BLK_SIZE + xx, y + yy, ch, c) +
                  s8_val(par[c]));
  if (op == BLK2_UP)
    return u8_sat((int)blk2_sample(im, x + xx, y - BLK_SIZE + yy, ch, c) +
                  s8_val(par[c]));
  if (op == BLK2_CAUSAL)
    return blk2_causal_img(im, x + xx, y + yy, ch, c);
  if (op == BLK2_PDM)
    return pdm_pred_img(im, x, y, bw, bh, BLK_SIZE, xx, yy, ch, c, par);
  return 0;
}

static uint8_t blk2_pred_buf(const uint8_t *pix, uint32_t w, uint32_t x,
                             uint32_t y, uint32_t bw, uint32_t bh, uint32_t xx,
                             uint32_t yy, int ch, int c, uint8_t op,
                             const uint8_t *par) {
  if (op == BLK2_FLAT)
    return par[c];
  if (op == BLK2_GRAD)
    return blk2_grad(par, bw, bh, xx, yy, c);
  if (op == BLK2_LEFT) {
    uint8_t v = pix[blk2_i(w, ch, x - BLK_SIZE + xx, y + yy, c)];
    return u8_sat((int)v + s8_val(par[c]));
  }
  if (op == BLK2_UP) {
    uint8_t v = pix[blk2_i(w, ch, x + xx, y - BLK_SIZE + yy, c)];
    return u8_sat((int)v + s8_val(par[c]));
  }
  if (op == BLK2_CAUSAL)
    return blk2_causal_buf(pix, w, ch, x + xx, y + yy, c);
  if (op == BLK2_PDM)
    return pdm_pred_buf(pix, w, x, y, bw, bh, BLK_SIZE, xx, yy, ch, c, par);
  return 0;
}

static uint64_t blk2_rscore(unsigned d) {
  unsigned m = d <= 128u ? d : 256u - d;
  if (!m)
    return 0;
  if (m <= 1u)
    return 1;
  if (m <= 3u)
    return 2;
  if (m <= 7u)
    return 3;
  if (m <= 15u)
    return 4;
  if (m <= 31u)
    return 5;
  if (m <= 63u)
    return 6;
  if (m <= 127u)
    return 7;
  return 8;
}

static void blk2_flat_params(const Image *im, uint32_t x, uint32_t y,
                             uint32_t bw, uint32_t bh, int ch, uint8_t *par) {
  uint32_t n = bw * bh;
  for (int c = 0; c < ch; ++c) {
    uint32_t sum = 0;
    for (uint32_t yy = 0; yy < bh; ++yy)
      for (uint32_t xx = 0; xx < bw; ++xx)
        sum += blk2_sample(im, x + xx, y + yy, ch, c);
    par[c] = (uint8_t)((sum + n / 2u) / n);
  }
}

static void blk2_grad_params(const Image *im, uint32_t x, uint32_t y,
                             uint32_t bw, uint32_t bh, int ch, uint8_t *par) {
  uint32_t x1 = x + bw - 1u;
  uint32_t y1 = y + bh - 1u;
  for (int c = 0; c < ch; ++c) {
    uint8_t *p = par + (size_t)c * 4u;
    p[0] = blk2_sample(im, x, y, ch, c);
    p[1] = blk2_sample(im, x1, y, ch, c);
    p[2] = blk2_sample(im, x, y1, ch, c);
    p[3] = blk2_sample(im, x1, y1, ch, c);
  }
}

static void blk2_delta_params(const Image *im, uint32_t x, uint32_t y,
                              uint32_t sx, uint32_t sy, uint32_t bw,
                              uint32_t bh, int ch, uint8_t *par) {
  int n = (int)(bw * bh);
  for (int c = 0; c < ch; ++c) {
    int64_t sum = 0;
    for (uint32_t yy = 0; yy < bh; ++yy) {
      for (uint32_t xx = 0; xx < bw; ++xx) {
        int d = (int)blk2_sample(im, x + xx, y + yy, ch, c) -
                (int)blk2_sample(im, sx + xx, sy + yy, ch, c);
        if (d > 128)
          d -= 256;
        else if (d < -128)
          d += 256;
        sum += d;
      }
    }
    int v = sum >= 0 ? (int)((sum + n / 2) / n) : -(int)((-sum + n / 2) / n);
    if (v < -128)
      v = -128;
    if (v > 127)
      v = 127;
    par[c] = (uint8_t)(v & 255);
  }
}

static uint64_t blk2_score(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                           uint32_t bh, int ch, uint8_t op,
                           const uint8_t *par) {
  uint64_t s = (1u + (uint64_t)blk2_parn(op, ch)) * 20u;
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      for (int c = 0; c < ch; ++c) {
        unsigned a = blk2_sample(im, x + xx, y + yy, ch, c);
        unsigned p = blk2_pred_img(im, x, y, bw, bh, xx, yy, ch, c, op, par);
        s += blk2_rscore((a - p) & 255u);
      }
    }
  }
  return s;
}

static uint64_t blk2_score_linear(const Image *im, uint32_t x, uint32_t y,
                                  uint32_t bw, uint32_t bh, int ch) {
  uint64_t s = 20u;
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      for (int c = 0; c < ch; ++c) {
        unsigned a = blk2_sample(im, x + xx, y + yy, ch, c);
        unsigned p = blk2_linear_img(im, x + xx, y + yy, ch, c);
        s += blk2_rscore((a - p) & 255u);
      }
    }
  }
  return s;
}

static uint64_t pdm_score(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                          uint32_t bh, int ch, const uint8_t *par,
                          uint32_t step) {
  uint64_t s = (1u + (uint64_t)blk2_parn(BLK2_PDM, ch)) * 20u;
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      for (int c = 0; c < ch; ++c) {
        unsigned a = blk2_sample(im, x + xx, y + yy, ch, c);
        unsigned p = pdm_pred_img(im, x, y, bw, bh, step, xx, yy, ch, c, par);
        s += blk2_rscore((a - p) & 255u);
      }
    }
  }
  return s;
}

static uint64_t pdm_min(uint64_t a, uint64_t b) { return a < b ? a : b; }

static uint8_t pdm_pair(uint64_t a, uint64_t b) {
  if (a == UINT64_MAX && b == UINT64_MAX)
    return 8;
  if (a == UINT64_MAX)
    return 15;
  if (b == UINT64_MAX)
    return 0;
  uint64_t d = a + b;
  if (!d)
    return 8;
  uint64_t v = (a * 15u + d / 2u) / d;
  return v > 15u ? 15u : (uint8_t)v;
}

static uint8_t pdm_adj(uint8_t v, int d) {
  int n = (int)v + d;
  if (n < 0)
    return 0;
  if (n > 15)
    return 15;
  return (uint8_t)n;
}

static void pdm_set(uint8_t *par, uint8_t q0, uint8_t q1, uint8_t q2,
                    uint8_t q3, uint8_t q4, const uint8_t *flat,
                    const uint8_t *grad, const uint8_t *left, const uint8_t *up,
                    int ch) {
  pdm_pack(par, q0, q1, q2, q3, q4);
  memcpy(par + 3u, flat, (size_t)ch);
  memcpy(par + 3u + (size_t)ch, grad, (size_t)ch * 4u);
  memcpy(par + 3u + (size_t)ch * 5u, left, (size_t)ch);
  memcpy(par + 3u + (size_t)ch * 6u, up, (size_t)ch);
}

static void pdm_try(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                    uint32_t bh, int ch, uint32_t step, const uint8_t *flat,
                    const uint8_t *grad, const uint8_t *left, const uint8_t *up,
                    uint8_t q0, uint8_t q1, uint8_t q2, uint8_t q3, uint8_t q4,
                    uint64_t *best, uint8_t *best_par) {
  uint8_t p[BLK2_MAX_PAR];
  pdm_set(p, q0, q1, q2, q3, q4, flat, grad, left, up, ch);
  uint64_t s = pdm_score(im, x, y, bw, bh, ch, p, step);
  if (s < *best) {
    *best = s;
    memcpy(best_par, p, blk2_parn(BLK2_PDM, ch));
  }
}

static int pdm_params_step(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                           uint32_t bh, int ch, uint32_t step, int require_win,
                           uint8_t *par, uint64_t *mix_score) {
  uint8_t tmp[BLK2_MAX_PAR];
  uint8_t flat[4] = {0, 0, 0, 0};
  uint8_t grad[16];
  uint8_t left[4] = {0, 0, 0, 0};
  uint8_t up[4] = {0, 0, 0, 0};
  uint64_t s[6];

  blk2_flat_params(im, x, y, bw, bh, ch, flat);
  s[0] = blk2_score(im, x, y, bw, bh, ch, BLK2_FLAT, flat);
  blk2_grad_params(im, x, y, bw, bh, ch, grad);
  s[1] = blk2_score(im, x, y, bw, bh, ch, BLK2_GRAD, grad);
  if (x >= step) {
    blk2_delta_params(im, x, y, x - step, y, bw, bh, ch, left);
    pdm_set(tmp, 0, 15, 0, 0, 0, flat, grad, left, up, ch);
    s[2] = pdm_score(im, x, y, bw, bh, ch, tmp, step);
  } else {
    s[2] = UINT64_MAX;
  }
  if (y >= step) {
    blk2_delta_params(im, x, y, x, y - step, bw, bh, ch, up);
    pdm_set(tmp, 0, 0, 0, 0, 0, flat, grad, left, up, ch);
    s[3] = pdm_score(im, x, y, bw, bh, ch, tmp, step);
  } else {
    s[3] = UINT64_MAX;
  }
  memset(tmp, 0, sizeof(tmp));
  s[4] = blk2_score(im, x, y, bw, bh, ch, BLK2_CAUSAL, tmp);
  s[5] = blk2_score_linear(im, x, y, bw, bh, ch);

  uint64_t best = s[0];
  for (int i = 1; i < 6; ++i)
    if (s[i] < best)
      best = s[i];
  uint64_t block = pdm_min(s[0], s[1]);
  uint64_t axis = pdm_min(s[2], s[3]);
  uint64_t edge = pdm_min(s[4], s[5]);
  uint64_t nb = pdm_min(axis, edge);
  uint8_t q0 = pdm_pair(nb, block);
  uint8_t q1 = pdm_pair(s[3], s[2]);
  uint8_t q2 = pdm_pair(s[0], s[1]);
  uint8_t q3 = pdm_pair(axis, edge);
  uint8_t q4 = pdm_pair(s[4], s[5]);
  uint64_t ps = UINT64_MAX;
  uint8_t best_par[BLK2_MAX_PAR];
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, q0, q1, q2, q3, q4,
          &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, 15, q1, 0, q3, q4,
          &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, 15, q1, 15, q3, q4,
          &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, 0, 15, q2, 0, q4,
          &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, 0, 0, q2, 0, q4,
          &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, 0, q1, q2, 15, 0,
          &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, 0, q1, q2, 15, 15,
          &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, 8, q1, q2, 8, q4,
          &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, pdm_adj(q0, -2), q1,
          q2, q3, q4, &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, pdm_adj(q0, 2), q1,
          q2, q3, q4, &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, q0, q1,
          pdm_adj(q2, -2), q3, q4, &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, q0, q1,
          pdm_adj(q2, 2), q3, q4, &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, q0, q1, q2,
          pdm_adj(q3, -2), q4, &ps, best_par);
  pdm_try(im, x, y, bw, bh, ch, step, flat, grad, left, up, q0, q1, q2,
          pdm_adj(q3, 2), q4, &ps, best_par);
  if (ps == UINT64_MAX || (require_win && ps + ps / 64u >= best))
    return 0;
  memcpy(par, best_par, blk2_parn(BLK2_PDM, ch));
  *mix_score = ps;
  return 1;
}

static int pdm_params(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                      uint32_t bh, int ch, uint8_t *par, uint64_t *mix_score) {
  return pdm_params_step(im, x, y, bw, bh, ch, BLK_SIZE, 1, par, mix_score);
}

static void cf_base_img(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                        uint32_t bh, int ch, uint8_t *par) {
  uint8_t *flat = par + 3u;
  uint8_t *grad = flat + (size_t)ch;
  uint8_t *left = grad + (size_t)ch * 4u;
  uint8_t *up = left + (size_t)ch;
  for (int c = 0; c < ch; ++c) {
    uint32_t sum = 0;
    uint32_t n = 0;
    if (y) {
      for (uint32_t xx = 0; xx < bw; ++xx) {
        sum += blk2_sample(im, x + xx, y - 1u, ch, c);
        ++n;
      }
    }
    if (x) {
      for (uint32_t yy = 0; yy < bh; ++yy) {
        sum += blk2_sample(im, x - 1u, y + yy, ch, c);
        ++n;
      }
    }
    uint8_t f = n ? (uint8_t)((sum + n / 2u) / n) : 0;
    uint8_t tl = x && y ? blk2_sample(im, x - 1u, y - 1u, ch, c) : f;
    uint8_t tr = y ? blk2_sample(im, x + bw - 1u, y - 1u, ch, c) : f;
    uint8_t bl = x ? blk2_sample(im, x - 1u, y + bh - 1u, ch, c) : f;
    flat[c] = f;
    grad[(size_t)c * 4u + 0u] = tl;
    grad[(size_t)c * 4u + 1u] = tr;
    grad[(size_t)c * 4u + 2u] = bl;
    grad[(size_t)c * 4u + 3u] = u8_sat((int)tr + (int)bl - (int)tl);
    left[c] = 0;
    up[c] = 0;
  }
}

static void cf_base_buf(const uint8_t *pix, uint32_t w, uint32_t x, uint32_t y,
                        uint32_t bw, uint32_t bh, int ch, uint8_t *par) {
  uint8_t *flat = par + 3u;
  uint8_t *grad = flat + (size_t)ch;
  uint8_t *left = grad + (size_t)ch * 4u;
  uint8_t *up = left + (size_t)ch;
  for (int c = 0; c < ch; ++c) {
    uint32_t sum = 0;
    uint32_t n = 0;
    if (y) {
      for (uint32_t xx = 0; xx < bw; ++xx) {
        sum += blk2_sample_buf(pix, w, ch, x + xx, y - 1u, c);
        ++n;
      }
    }
    if (x) {
      for (uint32_t yy = 0; yy < bh; ++yy) {
        sum += blk2_sample_buf(pix, w, ch, x - 1u, y + yy, c);
        ++n;
      }
    }
    uint8_t f = n ? (uint8_t)((sum + n / 2u) / n) : 0;
    uint8_t tl = x && y ? blk2_sample_buf(pix, w, ch, x - 1u, y - 1u, c) : f;
    uint8_t tr = y ? blk2_sample_buf(pix, w, ch, x + bw - 1u, y - 1u, c) : f;
    uint8_t bl = x ? blk2_sample_buf(pix, w, ch, x - 1u, y + bh - 1u, c) : f;
    flat[c] = f;
    grad[(size_t)c * 4u + 0u] = tl;
    grad[(size_t)c * 4u + 1u] = tr;
    grad[(size_t)c * 4u + 2u] = bl;
    grad[(size_t)c * 4u + 3u] = u8_sat((int)tr + (int)bl - (int)tl);
    left[c] = 0;
    up[c] = 0;
  }
}

static uint64_t cf_score(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                         uint32_t bh, int ch, const uint8_t *par) {
  uint64_t s = 80u;
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      for (int c = 0; c < ch; ++c) {
        unsigned a = blk2_sample(im, x + xx, y + yy, ch, c);
        unsigned p =
            pdm_pred_img(im, x, y, bw, bh, CF_SIZE, xx, yy, ch, c, par);
        s += blk2_rscore((a - p) & 255u);
      }
    }
  }
  return s;
}

static uint64_t cf_score_sample(const Image *im, uint32_t x, uint32_t y,
                                uint32_t bw, uint32_t bh, int ch,
                                const uint8_t *par) {
  uint32_t sx = bw > 16u ? bw / 8u : 1u;
  uint32_t sy = bh > 16u ? bh / 8u : 1u;
  uint64_t s = 0;
  for (uint32_t yy = 0; yy < bh; yy += sy) {
    for (uint32_t xx = 0; xx < bw; xx += sx) {
      for (int c = 0; c < ch; ++c) {
        unsigned a = blk2_sample(im, x + xx, y + yy, ch, c);
        unsigned p =
            pdm_pred_img(im, x, y, bw, bh, CF_SIZE, xx, yy, ch, c, par);
        s += blk2_rscore((a - p) & 255u);
      }
    }
  }
  return s;
}

static void cf_try(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                   uint32_t bh, int ch, const uint8_t *base, uint8_t q0,
                   uint8_t q1, uint8_t q2, uint8_t q3, uint8_t q4,
                   uint64_t *best, uint8_t *best_coord) {
  uint8_t p[BLK2_MAX_PAR];
  memcpy(p, base, blk2_parn(BLK2_PDM, ch));
  pdm_pack(p, q0, q1, q2, q3, q4);
  uint64_t s = cf_score(im, x, y, bw, bh, ch, p);
  if (s < *best) {
    *best = s;
    memcpy(best_coord, p, 3u);
  }
}

static int cf_params(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                     uint32_t bh, int ch, uint8_t *coord, uint64_t *mix_score,
                     uint64_t *causal_score) {
  uint8_t base[BLK2_MAX_PAR];
  memset(base, 0, sizeof(base));
  cf_base_img(im, x, y, bw, bh, ch, base);
  uint8_t p[BLK2_MAX_PAR];
  uint64_t s[6];
  memcpy(p, base, blk2_parn(BLK2_PDM, ch));
  pdm_pack(p, 15, 8, 0, 8, 8);
  s[0] = cf_score(im, x, y, bw, bh, ch, p);
  pdm_pack(p, 15, 8, 15, 8, 8);
  s[1] = cf_score(im, x, y, bw, bh, ch, p);
  pdm_pack(p, 0, 15, 0, 0, 0);
  s[2] = x >= CF_SIZE ? cf_score(im, x, y, bw, bh, ch, p) : UINT64_MAX;
  pdm_pack(p, 0, 0, 0, 0, 0);
  s[3] = y >= CF_SIZE ? cf_score(im, x, y, bw, bh, ch, p) : UINT64_MAX;
  pdm_pack(p, 0, 8, 0, 15, 0);
  s[4] = cf_score(im, x, y, bw, bh, ch, p);
  pdm_pack(p, 0, 8, 0, 15, 15);
  s[5] = cf_score(im, x, y, bw, bh, ch, p);
  uint64_t block = pdm_min(s[0], s[1]);
  uint64_t axis = pdm_min(s[2], s[3]);
  uint64_t edge = pdm_min(s[4], s[5]);
  uint64_t nb = pdm_min(axis, edge);
  uint8_t q0 = pdm_pair(nb, block);
  uint8_t q1 = pdm_pair(s[3], s[2]);
  uint8_t q2 = pdm_pair(s[0], s[1]);
  uint8_t q3 = pdm_pair(axis, edge);
  uint8_t q4 = pdm_pair(s[4], s[5]);
  uint64_t best = UINT64_MAX;
  uint8_t best_coord[3] = {0, 0, 0};
#define CF_BEST(sc, a, b, c, d, e)                                             \
  do {                                                                         \
    if ((sc) < best) {                                                         \
      best = (sc);                                                             \
      pdm_pack(best_coord, a, b, c, d, e);                                     \
    }                                                                          \
  } while (0)
  CF_BEST(s[0], 15, 8, 0, 8, 8);
  CF_BEST(s[1], 15, 8, 15, 8, 8);
  if (x >= CF_SIZE)
    CF_BEST(s[2], 0, 15, 0, 0, 0);
  if (y >= CF_SIZE)
    CF_BEST(s[3], 0, 0, 0, 0, 0);
  CF_BEST(s[4], 0, 8, 0, 15, 0);
  CF_BEST(s[5], 0, 8, 0, 15, 15);
#undef CF_BEST
  cf_try(im, x, y, bw, bh, ch, base, q0, q1, q2, q3, q4, &best, best_coord);
  cf_try(im, x, y, bw, bh, ch, base, pdm_adj(q0, -2), q1, q2, q3, q4, &best,
         best_coord);
  cf_try(im, x, y, bw, bh, ch, base, pdm_adj(q0, 2), q1, q2, q3, q4, &best,
         best_coord);
  cf_try(im, x, y, bw, bh, ch, base, q0, q1, pdm_adj(q2, -2), q3, q4, &best,
         best_coord);
  cf_try(im, x, y, bw, bh, ch, base, q0, q1, pdm_adj(q2, 2), q3, q4, &best,
         best_coord);
  memcpy(coord, best_coord, 3u);
  *mix_score = best;
  *causal_score = s[4];
  return best != UINT64_MAX;
}

static int probe_cf(const Image *im, int ch) {
  uint32_t rx = (im->width + CF_SIZE - 1u) / CF_SIZE;
  uint32_t ry = (im->height + CF_SIZE - 1u) / CF_SIZE;
  uint32_t xs = rx > 24u ? rx / 24u : 1u;
  uint32_t ys = ry > 24u ? ry / 24u : 1u;
  uint64_t best_total = 0;
  uint64_t causal_total = 0;
  uint32_t n = 0;
  for (uint32_t by = 0; by < ry; by += ys) {
    for (uint32_t bx = 0; bx < rx; bx += xs) {
      uint32_t x = bx * CF_SIZE;
      uint32_t y = by * CF_SIZE;
      uint32_t bw = block_extent(im->width, x, CF_SIZE);
      uint32_t bh = block_extent(im->height, y, CF_SIZE);
      uint8_t base[BLK2_MAX_PAR];
      uint8_t p[BLK2_MAX_PAR];
      memset(base, 0, sizeof(base));
      cf_base_img(im, x, y, bw, bh, ch, base);
      memcpy(p, base, blk2_parn(BLK2_PDM, ch));
      pdm_pack(p, 15, 8, 0, 8, 8);
      uint64_t best = cf_score_sample(im, x, y, bw, bh, ch, p);
      pdm_pack(p, 15, 8, 15, 8, 8);
      best = pdm_min(best, cf_score_sample(im, x, y, bw, bh, ch, p));
      if (x >= CF_SIZE) {
        pdm_pack(p, 0, 15, 0, 0, 0);
        best = pdm_min(best, cf_score_sample(im, x, y, bw, bh, ch, p));
      }
      if (y >= CF_SIZE) {
        pdm_pack(p, 0, 0, 0, 0, 0);
        best = pdm_min(best, cf_score_sample(im, x, y, bw, bh, ch, p));
      }
      pdm_pack(p, 0, 8, 0, 15, 0);
      uint64_t causal = cf_score_sample(im, x, y, bw, bh, ch, p);
      best = pdm_min(best, causal);
      pdm_pack(p, 0, 8, 0, 15, 15);
      best = pdm_min(best, cf_score_sample(im, x, y, bw, bh, ch, p));
      best_total += best;
      causal_total += causal;
      ++n;
    }
  }
  return n >= 4u && causal_total && best_total * 100u < causal_total * 86u;
}

static void cf_residual(const Image *im, uint8_t *res, uint32_t x, uint32_t y,
                        uint32_t bw, uint32_t bh, int ch,
                        const uint8_t *coord) {
  uint8_t par[BLK2_MAX_PAR];
  memset(par, 0, sizeof(par));
  cf_base_img(im, x, y, bw, bh, ch, par);
  memcpy(par, coord, 3u);
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      for (int c = 0; c < ch; ++c) {
        uint8_t a = blk2_sample(im, x + xx, y + yy, ch, c);
        uint8_t p = pdm_pred_img(im, x, y, bw, bh, CF_SIZE, xx, yy, ch, c, par);
        res[blk2_i(im->width, ch, x + xx, y + yy, c)] =
            fold_delta((int)a - (int)p);
      }
    }
  }
}

static int finish_region_payload(Buf *out, const char *magic, int block_size,
                                 int ch, Buf *table, uint8_t *res,
                                 const Image *im, int stream_search,
                                 const char *kind) {
  uint8_t *z = NULL;
  size_t zn = 0;
  int search = stream_search > 4 ? 4 : stream_search;
  int err = stream_encode_threads(res, im->width, im->height, ch, search,
                                  g_threads ? g_threads : 1u, &z, &zn);
  free(res);
  if (err == STREAM_E_DIM) {
    buf_free(table);
    return -1;
  }
  if (err != STREAM_OK) {
    buf_free(table);
    set_err_status(stream_failure_status(err, 0),
                   "%s residual encode failed: %s", kind, stream_strerror(err));
    return 0;
  }
  int ok = buf_append(out, magic, 4) && buf_u8(out, (uint8_t)block_size) &&
           buf_u8(out, (uint8_t)ch) && buf_u16le(out, 0) &&
           buf_u32le(out, (uint32_t)table->size) &&
           buf_u64le(out, (uint64_t)zn) &&
           buf_append(out, table->data, table->size) && buf_append(out, z, zn);
  stream_free(z);
  buf_free(table);
  return ok;
}

static int mk_cf_regions(const Image *im, int ch, int stream_search, Buf *out) {
  size_t pixels = 0, bytes = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels) ||
      !mulok(pixels, (size_t)ch, &bytes))
    return 0;
  uint8_t *res = (uint8_t *)xmalloc(bytes);
  if (!res)
    return 0;
  Buf table = {0};
  uint64_t total_cf = 0;
  uint64_t total_causal = 0;
  for (uint32_t y = 0; y < im->height; y += CF_SIZE) {
    for (uint32_t x = 0; x < im->width; x += CF_SIZE) {
      uint32_t bw = block_extent(im->width, x, CF_SIZE);
      uint32_t bh = block_extent(im->height, y, CF_SIZE);
      uint8_t coord[3];
      uint64_t score = 0;
      uint64_t causal = 0;
      if (!cf_params(im, x, y, bw, bh, ch, coord, &score, &causal)) {
        free(res);
        buf_free(&table);
        return 0;
      }
      if (!buf_append(&table, coord, 3u)) {
        free(res);
        buf_free(&table);
        return 0;
      }
      cf_residual(im, res, x, y, bw, bh, ch, coord);
      total_cf += score;
      total_causal += causal;
    }
  }
  if (!total_causal || total_cf >= total_causal - total_causal / 300u) {
    free(res);
    buf_free(&table);
    return -1;
  }
  if (table.size > UINT32_MAX) {
    free(res);
    buf_free(&table);
    set_err("coordinate table is too large");
    return 0;
  }
  return finish_region_payload(out, "QCF1", CF_SIZE, ch, &table, res, im,
                               stream_search, "coordinate");
}

static int blk2_choose(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                       uint32_t bh, int ch, uint8_t *op, uint8_t *par,
                       uint64_t *best_score, uint64_t *base_score) {
  uint8_t tmp[BLK2_MAX_PAR];
  memset(tmp, 0, sizeof(tmp));
  uint64_t best = blk2_score(im, x, y, bw, bh, ch, BLK2_ZERO, tmp);
  uint8_t best_op = BLK2_ZERO;
  uint8_t best_par[BLK2_MAX_PAR];
  memset(best_par, 0, sizeof(best_par));
  *base_score = best;

  blk2_flat_params(im, x, y, bw, bh, ch, tmp);
  uint64_t s = blk2_score(im, x, y, bw, bh, ch, BLK2_FLAT, tmp);
  if (s < best) {
    best = s;
    best_op = BLK2_FLAT;
    memcpy(best_par, tmp, (size_t)ch);
  }

  blk2_grad_params(im, x, y, bw, bh, ch, tmp);
  s = blk2_score(im, x, y, bw, bh, ch, BLK2_GRAD, tmp);
  if (s < best) {
    best = s;
    best_op = BLK2_GRAD;
    memcpy(best_par, tmp, (size_t)ch * 4u);
  }

  if (x >= BLK_SIZE) {
    blk2_delta_params(im, x, y, x - BLK_SIZE, y, bw, bh, ch, tmp);
    s = blk2_score(im, x, y, bw, bh, ch, BLK2_LEFT, tmp);
    if (s < best) {
      best = s;
      best_op = BLK2_LEFT;
      memcpy(best_par, tmp, (size_t)ch);
    }
  }
  if (y >= BLK_SIZE) {
    blk2_delta_params(im, x, y, x, y - BLK_SIZE, bw, bh, ch, tmp);
    s = blk2_score(im, x, y, bw, bh, ch, BLK2_UP, tmp);
    if (s < best) {
      best = s;
      best_op = BLK2_UP;
      memcpy(best_par, tmp, (size_t)ch);
    }
  }
  s = blk2_score(im, x, y, bw, bh, ch, BLK2_CAUSAL, tmp);
  if (s < best) {
    best = s;
    best_op = BLK2_CAUSAL;
  }
  uint8_t pdm[BLK2_MAX_PAR];
  uint64_t ps = 0;
  int has_pdm = pdm_params(im, x, y, bw, bh, ch, pdm, &ps);
  if (has_pdm && ps < best) {
    best = ps;
    best_op = BLK2_PDM;
    memcpy(best_par, pdm, blk2_parn(BLK2_PDM, ch));
  }

  *op = best_op;
  memcpy(par, best_par, blk2_parn(best_op, ch));
  *best_score = best;
  return 1;
}

static void blk2_residual(const Image *im, uint8_t *res, uint32_t x, uint32_t y,
                          uint32_t bw, uint32_t bh, int ch, uint8_t op,
                          const uint8_t *par) {
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      for (int c = 0; c < ch; ++c) {
        uint8_t a = blk2_sample(im, x + xx, y + yy, ch, c);
        uint8_t p = blk2_pred_img(im, x, y, bw, bh, xx, yy, ch, c, op, par);
        res[blk2_i(im->width, ch, x + xx, y + yy, c)] =
            fold_delta((int)a - (int)p);
      }
    }
  }
}

static int mk_blocks2(const Image *im, int ch, int stream_search, Buf *out) {
  size_t pixels = 0, bytes = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels) ||
      !mulok(pixels, (size_t)ch, &bytes))
    return 0;
  uint8_t *res = (uint8_t *)xmalloc(bytes);
  if (!res)
    return 0;
  Buf table = {0};
  uint64_t total_best = 0;
  uint64_t total_base = 0;
  for (uint32_t y = 0; y < im->height; y += BLK_SIZE) {
    for (uint32_t x = 0; x < im->width; x += BLK_SIZE) {
      uint32_t bw = block_extent(im->width, x, BLK_SIZE);
      uint32_t bh = block_extent(im->height, y, BLK_SIZE);
      uint8_t op = 0;
      uint8_t par[BLK2_MAX_PAR];
      uint64_t best_score = 0;
      uint64_t base_score = 0;
      if (!blk2_choose(im, x, y, bw, bh, ch, &op, par, &best_score,
                       &base_score)) {
        free(res);
        buf_free(&table);
        return 0;
      }
      size_t parn = blk2_parn(op, ch);
      if (!buf_u8(&table, op) || !buf_append(&table, par, parn)) {
        free(res);
        buf_free(&table);
        return 0;
      }
      blk2_residual(im, res, x, y, bw, bh, ch, op, par);
      total_best += best_score;
      total_base += base_score;
    }
  }
  if (!total_base || total_best >= total_base - total_base / 100u) {
    free(res);
    buf_free(&table);
    return -1;
  }
  if (table.size > UINT32_MAX) {
    free(res);
    buf_free(&table);
    set_err("block table is too large");
    return 0;
  }
  return finish_region_payload(out, "QBL2", BLK_SIZE, ch, &table, res, im,
                               stream_search, "block");
}

static void pdm_residual(const Image *im, uint8_t *res, uint32_t x, uint32_t y,
                         uint32_t bw, uint32_t bh, int ch, const uint8_t *par) {
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      for (int c = 0; c < ch; ++c) {
        uint8_t a = blk2_sample(im, x + xx, y + yy, ch, c);
        uint8_t p =
            pdm_pred_img(im, x, y, bw, bh, PDM_SIZE, xx, yy, ch, c, par);
        res[blk2_i(im->width, ch, x + xx, y + yy, c)] =
            fold_delta((int)a - (int)p);
      }
    }
  }
}

static int mk_pdm_regions(const Image *im, int ch, int stream_search,
                          Buf *out) {
  size_t pixels = 0, bytes = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels) ||
      !mulok(pixels, (size_t)ch, &bytes))
    return 0;
  uint8_t *res = (uint8_t *)xmalloc(bytes);
  if (!res)
    return 0;
  Buf table = {0};
  uint64_t total_pdm = 0;
  uint64_t total_causal = 0;
  uint8_t tmp[BLK2_MAX_PAR];
  memset(tmp, 0, sizeof(tmp));
  for (uint32_t y = 0; y < im->height; y += PDM_SIZE) {
    for (uint32_t x = 0; x < im->width; x += PDM_SIZE) {
      uint32_t bw = block_extent(im->width, x, PDM_SIZE);
      uint32_t bh = block_extent(im->height, y, PDM_SIZE);
      uint8_t par[BLK2_MAX_PAR];
      uint64_t score = 0;
      if (!pdm_params_step(im, x, y, bw, bh, ch, PDM_SIZE, 0, par, &score)) {
        free(res);
        buf_free(&table);
        return 0;
      }
      size_t parn = blk2_parn(BLK2_PDM, ch);
      if (!buf_append(&table, par, parn)) {
        free(res);
        buf_free(&table);
        return 0;
      }
      pdm_residual(im, res, x, y, bw, bh, ch, par);
      total_pdm += score;
      total_causal += blk2_score(im, x, y, bw, bh, ch, BLK2_CAUSAL, tmp);
    }
  }
  if (!total_causal || total_pdm >= total_causal - total_causal / 200u) {
    free(res);
    buf_free(&table);
    return -1;
  }
  if (table.size > UINT32_MAX) {
    free(res);
    buf_free(&table);
    set_err("pdm table is too large");
    return 0;
  }
  return finish_region_payload(out, "QPD1", PDM_SIZE, ch, &table, res, im,
                               stream_search, "pdm");
}

static int mk_blocks(const Image *im, int ch, Buf *out) {
  if (!buf_append(out, "QBL1", 4) || !buf_u8(out, (uint8_t)BLK_SIZE) ||
      !buf_u8(out, (uint8_t)ch) || !buf_u16le(out, 0))
    return 0;
  uint32_t blocks_x = (im->width + BLK_SIZE - 1u) / BLK_SIZE;
  uint32_t blocks_y = (im->height + BLK_SIZE - 1u) / BLK_SIZE;
  size_t blocks = 0;
  BlockRefTable refs = {0};
  if (!mulok((size_t)blocks_x, (size_t)blocks_y, &blocks) ||
      !blk_refs_init(&refs, blocks)) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  size_t index = 0;
  int extended = 0;
  for (uint32_t y = 0; y < im->height; y += BLK_SIZE) {
    for (uint32_t x = 0; x < im->width; x += BLK_SIZE) {
      uint32_t bw = block_extent(im->width, x, BLK_SIZE);
      uint32_t bh = block_extent(im->height, y, BLK_SIZE);
      size_t source =
          blk_refs_find(&refs, im, x, y, bw, bh, ch, index, blocks_x);
      size_t distance = source == SIZE_MAX ? 0u : index - source;
      if (!blk_write_one(out, im, x, y, ch, distance, &extended)) {
        free(refs.entries);
        return 0;
      }
      ++index;
    }
  }
  free(refs.entries);
  if (extended)
    memcpy(out->data, "QBR1", 4);
  return 1;
}

static uint64_t blk_mix(uint64_t v) {
  v ^= v >> 30;
  v *= UINT64_C(0xbf58476d1ce4e5b9);
  v ^= v >> 27;
  v *= UINT64_C(0x94d049bb133111eb);
  return v ^ (v >> 31);
}

static uint64_t blk_key(const Image *im, uint32_t x, uint32_t y, uint32_t bw,
                        uint32_t bh, int ch) {
  uint32_t px[5] = {0, bw - 1u, 0, bw - 1u, bw >> 1};
  uint32_t py[5] = {0, 0, bh - 1u, bh - 1u, bh >> 1};
  uint64_t key = ((uint64_t)bw << 32) | bh;
  for (int i = 0; i < 5; ++i)
    key = blk_mix(key ^ blk_color(im, x + px[i], y + py[i], ch));
  return key;
}

static int blk_refs_init(BlockRefTable *table, size_t blocks) {
  size_t target = blocks < 32768u ? blocks * 2u : 65536u;
  size_t cap = 2u;
  while (cap < target)
    cap <<= 1;
  table->entries = (BlockRefEntry *)calloc(cap, sizeof(*table->entries));
  if (!table->entries)
    return 0;
  table->cap = cap;
  return 1;
}

static size_t blk_refs_find(BlockRefTable *table, const Image *im, uint32_t x,
                            uint32_t y, uint32_t bw, uint32_t bh, int ch,
                            size_t index, uint32_t blocks_x) {
  uint64_t key = blk_key(im, x, y, bw, bh, ch);
  size_t slot = (size_t)key & (table->cap - 1u);
  size_t replace = slot;
  size_t found = SIZE_MAX;
  /* a fixed probe count keeps crafted collisions from changing encode cost */
  for (size_t probe = 0; probe < 4u; ++probe) {
    size_t at = (slot + probe) & (table->cap - 1u);
    BlockRefEntry *entry = &table->entries[at];
    if (!entry->index_plus_one) {
      replace = at;
      break;
    }
    if (entry->key == key) {
      size_t source = entry->index_plus_one - 1u;
      uint32_t sx = (uint32_t)(source % blocks_x) * BLK_SIZE;
      uint32_t sy = (uint32_t)(source / blocks_x) * BLK_SIZE;
      if (block_extent(im->width, sx, BLK_SIZE) == bw &&
          block_extent(im->height, sy, BLK_SIZE) == bh &&
          blk_same(im, x, y, sx, sy, bw, bh, ch)) {
        found = source;
        replace = at;
        break;
      }
    }
  }
  table->entries[replace].key = key;
  table->entries[replace].index_plus_one = index + 1u;
  return found;
}

static int try_block_payload(Candidate *best, Buf *payload, int built,
                             int block_size) {
  int ok = built < 0 ||
           (built > 0 && try_store_owned(best, payload, MODE_BLOCKS,
                                         TRANSFORM_IDENTITY, block_size, 0));
  buf_free(payload);
  return ok;
}

static int try_blocks_base(Candidate *best, const Image *im, int ch) {
  Buf payload = {0};
  return try_block_payload(best, &payload, mk_blocks(im, ch, &payload),
                           BLK_SIZE);
}

static int try_blocks_ex(Candidate *best, const Image *im, int ch,
                         int stream_search, int skip_base) {
  size_t pixels = 0, compact = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels) ||
      !mulok(pixels, (size_t)ch, &compact))
    return 0;
  if (best->compressed && best->compressed_size < compact / 8u)
    return 1;
  if (!skip_base && !try_blocks_base(best, im, ch))
    return 0;
  if (pixels <= 16000000u && probe_cf(im, ch)) {
    Buf cf = {0};
    if (!try_block_payload(best, &cf, mk_cf_regions(im, ch, stream_search, &cf),
                           CF_SIZE))
      return 0;
  }
  if (pixels > 4000000u ||
      (best->compressed && best->compressed_size < compact / 8u))
    return 1;
  Buf payload2 = {0};
  if (!try_block_payload(best, &payload2,
                         mk_blocks2(im, ch, stream_search, &payload2),
                         BLK_SIZE))
    return 0;
  Buf pdm = {0};
  if (!try_block_payload(best, &pdm,
                         mk_pdm_regions(im, ch, stream_search, &pdm), BLK_SIZE))
    return 0;
  return 1;
}

static int try_blocks(Candidate *best, const Image *im, int ch,
                      int stream_search) {
  return try_blocks_ex(best, im, ch, stream_search, 0);
}

static int probe_blocks(const Image *im, int ch, unsigned *score_per_mille) {
  uint32_t bx_count = (im->width + BLK_SIZE - 1u) / BLK_SIZE;
  uint32_t by_count = (im->height + BLK_SIZE - 1u) / BLK_SIZE;
  if (bx_count < 2 && by_count < 2)
    return 0;
  uint32_t xs = bx_count > 48u ? bx_count / 48u : 1u;
  uint32_t ys = by_count > 48u ? by_count / 48u : 1u;
  unsigned hits = 0;
  unsigned total = 0;
  size_t expected = (size_t)bx_count * by_count;
  if (expected > 4096u)
    expected = 4096u;
  BlockRefTable refs = {0};
  int have_refs = blk_refs_init(&refs, expected);
  for (uint32_t by = 0; by < by_count; by += ys) {
    for (uint32_t bx = 0; bx < bx_count; bx += xs) {
      uint32_t x = bx * BLK_SIZE;
      uint32_t y = by * BLK_SIZE;
      uint32_t bw = block_extent(im->width, x, BLK_SIZE);
      uint32_t bh = block_extent(im->height, y, BLK_SIZE);
      uint32_t flat = 0;
      int useful = blk_flat(im, x, y, bw, bh, ch, &flat);
      if (!useful && bx && blk_same(im, x, y, x - BLK_SIZE, y, bw, bh, ch))
        useful = 1;
      if (!useful && by && blk_same(im, x, y, x, y - BLK_SIZE, bw, bh, ch))
        useful = 1;
      if (!useful && have_refs) {
        size_t index = (size_t)by * bx_count + bx;
        size_t source =
            blk_refs_find(&refs, im, x, y, bw, bh, ch, index, bx_count);
        if (source != SIZE_MAX)
          useful = 1;
      }
      if (!useful) {
        uint32_t colors[4];
        uint8_t indices[BLK_SIZE * BLK_SIZE];
        int count = 0;
        useful = blk_colors(im, x, y, bw, bh, colors, indices, 4, ch, &count);
      }
      hits += (unsigned)useful;
      ++total;
      if (total >= 4096u) {
        free(refs.entries);
        if (score_per_mille)
          *score_per_mille = total ? hits * 1000u / total : 0u;
        return hits * 8u >= total;
      }
    }
  }
  free(refs.entries);
  if (score_per_mille)
    *score_per_mille = total ? hits * 1000u / total : 0u;
  return total >= 16u && hits * 8u >= total;
}

#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
typedef struct {
  uint64_t hash;
  size_t position_plus_one;
} TexturedSpanRef;

/* Fixed-grid block probes miss repeated detail shifted away from tile
   boundaries. Sample at most 16 scanlines and 1,024 spans per line, require
   visible color variation, and verify every hash hit byte-for-byte. */
static int probe_textured_spans(const Image *im, unsigned *score_per_mille,
                                unsigned *eligible_spans) {
  enum {
    SPAN_PIXELS = 16,
    SAMPLE_ROWS = 16,
    SPANS_PER_ROW = 1024,
    TABLE_SIZE = 32768,
    MIN_ELIGIBLE = 4096,
    MIN_SCORE_PER_MILLE = 100
  };
  if (im->width < SPAN_PIXELS || !im->height)
    return 0;
  TexturedSpanRef *refs =
      (TexturedSpanRef *)calloc(TABLE_SIZE, sizeof(TexturedSpanRef));
  if (!refs)
    return 0;
  unsigned rows = im->height < SAMPLE_ROWS ? im->height : SAMPLE_ROWS;
  uint32_t windows = im->width - SPAN_PIXELS + 1u;
  uint32_t step = (windows + SPANS_PER_ROW - 1u) / SPANS_PER_ROW;
  unsigned hits = 0;
  unsigned eligible = 0;
  for (unsigned sample_y = 0; sample_y < rows; ++sample_y) {
    uint32_t y = rows == 1u
                     ? 0u
                     : (uint32_t)(((uint64_t)sample_y * (im->height - 1u)) /
                                  (rows - 1u));
    const uint8_t *row = im->rgba + (size_t)y * im->width * 4u;
    for (uint32_t x = 0; x < windows; x += step) {
      uint32_t c0 = ckey(row + (size_t)x * 4u);
      uint32_t c1 = ckey(row + (size_t)(x + 5u) * 4u);
      uint32_t c2 = ckey(row + (size_t)(x + 10u) * 4u);
      uint32_t c3 = ckey(row + (size_t)(x + 15u) * 4u);
      unsigned distinct = 1u;
      distinct += c1 != c0;
      distinct += c2 != c0 && c2 != c1;
      distinct += c3 != c0 && c3 != c1 && c3 != c2;
      if (distinct < 3u)
        continue;
      ++eligible;
      uint64_t hash = UINT64_C(1469598103934665603);
      for (unsigned i = 0; i < SPAN_PIXELS; ++i) {
        hash ^= ckey(row + (size_t)(x + i) * 4u);
        hash *= UINT64_C(1099511628211);
      }
      size_t slot = (size_t)hash & (TABLE_SIZE - 1u);
      size_t position = (size_t)y * im->width + x;
      for (;;) {
        TexturedSpanRef *ref = &refs[slot];
        if (!ref->position_plus_one) {
          ref->hash = hash;
          ref->position_plus_one = position + 1u;
          break;
        }
        if (ref->hash == hash) {
          size_t source = ref->position_plus_one - 1u;
          size_t source_y = source / im->width;
          size_t source_x = source - source_y * im->width;
          size_t distance = source_x > x ? source_x - x : x - source_x;
          if ((source_y != y || distance >= SPAN_PIXELS) &&
              !memcmp(im->rgba + source * 4u, row + (size_t)x * 4u,
                      SPAN_PIXELS * 4u)) {
            ++hits;
            break;
          }
        }
        slot = (slot + 1u) & (TABLE_SIZE - 1u);
      }
    }
  }
  free(refs);
  unsigned score = eligible ? hits * 1000u / eligible : 0u;
  if (score_per_mille)
    *score_per_mille = score;
  if (eligible_spans)
    *eligible_spans = eligible;
  return eligible >= MIN_ELIGIBLE && score >= MIN_SCORE_PER_MILLE;
}
#endif

static int try_separable(Candidate *best, const Image *im, int mode,
                         int *codecs, int codec_count, int *matched) {
  *matched = 0;
  Buf sep = {0};
  int sr = mk_sep(im, mode, &sep);
  if (sr < 0) {
    buf_free(&sep);
    return 0;
  }
  if (!sr) {
    buf_free(&sep);
    return 1;
  }
  *matched = 1;
  if (!try_pay(best, &sep, NULL, MODE_SEPARABLE, TRANSFORM_IDENTITY, mode, 0,
               codecs, codec_count)) {
    buf_free(&sep);
    return 0;
  }
  Buf delta = {0};
  if (!mk_sepd(&sep, im->width, im->height, mbpp(mode), &delta)) {
    buf_free(&delta);
    buf_free(&sep);
    return 0;
  }
  int ok = try_pay(best, &delta, NULL, MODE_SEPARABLE,
                   TRANSFORM_SEPARABLE_DELTA, mode, 0, codecs, codec_count);
  buf_free(&delta);
  buf_free(&sep);
  return ok;
}

static int looks_noisy(const Image *im, int mode, size_t pixels) {
  if (pixels < 4096u)
    return 0;
  size_t step = pixels / 262144u + 1u;
  uint64_t sum = 0;
  uint64_t n = 0;
  for (size_t i = step; i < pixels; i += step) {
    const uint8_t *a = im->rgba + i * 4u;
    const uint8_t *b = im->rgba + (i - step) * 4u;
    if (mode == MODE_GRAY || mode == MODE_GRAYA) {
      sum += (uint64_t)abs((int)a[0] - (int)b[0]);
      ++n;
    } else {
      sum += (uint64_t)abs((int)a[0] - (int)b[0]);
      sum += (uint64_t)abs((int)a[1] - (int)b[1]);
      sum += (uint64_t)abs((int)a[2] - (int)b[2]);
      n += 3u;
    }
  }
  return n && sum / n > 78u;
}

static int sparse_endpoint_alpha(const Image *im, size_t pixels) {
  size_t intermediate = 0;
  size_t intermediate_limit = pixels / 1000u;
  int has_transparent = 0;
  int has_opaque = 0;
  for (size_t i = 0; i < pixels; ++i) {
    uint8_t alpha = im->rgba[i * 4u + 3u];
    if (alpha == 0u)
      has_transparent = 1;
    else if (alpha == 255u)
      has_opaque = 1;
    else if (++intermediate > intermediate_limit)
      return 0;
  }
  return has_transparent && has_opaque;
}

static int enc_best_base(const Image *im, Candidate *best) {
#ifdef QLIC_STREAM_TRACE
  double encode_begin = qlic_candidate_trace_now();
#endif
  size_t pixels = 0;
  if (!mulok((size_t)im->width, (size_t)im->height, &pixels))
    return 0;
  int stream_search = 0;
  int quick_special = pixels <= 1000000u;
#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
  int codecs[] = {CODEC_STORE, CODEC_LZMS};
  int codec_count = 2;
#else
  int codecs[] = {CODEC_STORE};
  int codec_count = 1;
#endif

  /* probes only prune expensive work, exact encoded size still makes the choice
   */
  int mode = scan_mode(im);
  if (g_err[0])
    return 0;
  int channels = mode == MODE_GRAY ? 1 : mode == MODE_RGB ? 3 : 4;
  unsigned block_score_per_mille = 0;
  if (pixels >= 4096u && looks_noisy(im, mode, pixels) &&
      !probe_palette(im, 256u) &&
      !probe_blocks(im, channels, &block_score_per_mille)) {
    Buf samples = {0};
    if (!mk_samp(im, mode, TRANSFORM_IDENTITY, &samples) ||
        !try_store_owned(best, &samples, mode, TRANSFORM_IDENTITY_RAW, 0, 0)) {
      buf_free(&samples);
      return 0;
    }
    buf_free(&samples);
    return 1;
  }

  /* QST1 is already range coded, another compression pass is skipped */
  int stream_codecs[] = {CODEC_STORE};

  int use_stream = pixels <= 160000000u;
  int large_quick = pixels > 16000000u;
  /* noise makes deep prediction search expensive with little useful structure
   */
  int noisy = large_quick && looks_noisy(im, mode, pixels);

  uint32_t palette_probe_count = 4097u;
  int likely_palette =
      pixels >= 65536u && probe_palette_count(im, 4096u, &palette_probe_count);
  int compact_palette = likely_palette && palette_probe_count <= 256u;
  int separable = 0;
  int separable_checked = pixels <= 4000000u && !likely_palette;
  if (separable_checked) {
#ifdef QLIC_STREAM_TRACE
    double phase_begin = qlic_candidate_trace_now();
#endif
    if (!try_separable(best, im, mode, codecs, codec_count, &separable))
      return 0;
#ifdef QLIC_STREAM_TRACE
    qlic_candidate_trace_phase("separable", phase_begin);
#endif
  }
  if (separable && best->compressed) {
    Buf samples = {0};
    if (!mk_samp(im, mode, TRANSFORM_IDENTITY, &samples) ||
        !try_rle(best, &samples, NULL, mode, TRANSFORM_IDENTITY_RLE, 0, 0,
                 codecs, codec_count)) {
      buf_free(&samples);
      return 0;
    }
    buf_free(&samples);
    return 1;
  }
  int likely_blocks = 0;
#ifdef QLIC_STREAM_TRACE
  double block_probe_begin = qlic_candidate_trace_now();
#endif
  likely_blocks = pixels >= 65536u && !likely_palette &&
                  probe_blocks(im, channels, &block_score_per_mille);
#ifdef QLIC_STREAM_TRACE
  qlic_candidate_trace_phase("block-probe", block_probe_begin);
  if (qlic_candidate_trace_enabled())
    fprintf(stderr, "candidate-block-probe useful=%d score=%u\n", likely_blocks,
            block_score_per_mille);
#endif
  int tried_blocks_base = 0;
  if (likely_blocks && !separable) {
#ifdef QLIC_STREAM_TRACE
    double block_base_begin = qlic_candidate_trace_now();
#endif
    if (!try_blocks_base(best, im, channels))
      return 0;
#ifdef QLIC_STREAM_TRACE
    qlic_candidate_trace_phase("block-base", block_base_begin);
    if (qlic_candidate_trace_enabled())
      fprintf(stderr, "candidate-state name=block-base mode=%d bytes=%zu\n",
              best->compressed ? best->mode : -1, cand_size(best));
#endif
    tried_blocks_base = 1;
    size_t compact = 0;
    if (!mulok(pixels, (size_t)channels, &compact))
      return 0;
    size_t strong_limit = compact / 2u + compact / 10u;
    if (best->compressed && best->mode == MODE_BLOCKS &&
        best->compressed_size <= strong_limit && looks_noisy(im, mode, pixels))
      return 1;
  }
  int quick_native = pixels >= 65536u && !likely_palette && separable_checked &&
                     !separable && !likely_blocks && mode != MODE_GRAY;
  if (!noisy && use_stream) {
#ifdef QLIC_STREAM_TRACE
    double phase_begin = qlic_candidate_trace_now();
#endif
    if (!try_stream(best, im, mode, stream_search, stream_codecs, 1))
      return 0;
#ifdef QLIC_STREAM_TRACE
    qlic_candidate_trace_phase("native-stream", phase_begin);
    if (qlic_candidate_trace_enabled())
      fprintf(stderr, "candidate-state name=native-stream mode=%d bytes=%zu\n",
              best->compressed ? best->mode : -1, cand_size(best));
#endif
  }
  size_t raw_bytes = 0;
  if (!mulok(pixels, (size_t)mbpp(mode), &raw_bytes))
    return 0;
  int palette_sensitive = likely_palette && mode != MODE_GRAY;
  int compact_palette_candidate =
      compact_palette &&
      (mode == MODE_GRAY || palette_probe_count <= 8u ||
       (best->compressed && best->compressed_size <= raw_bytes / 25u));
  size_t native_limit = compact_palette && !compact_palette_candidate
                            ? raw_bytes / 10u
                        : palette_sensitive ? raw_bytes / 12u
                                            : raw_bytes - raw_bytes / 4u;
  int strong_native = !compact_palette_candidate && best->compressed &&
                      best->mode == MODE_NATIVE &&
                      best->compressed_size <= native_limit;
  int early_native = (quick_native || strong_native) && best->compressed;
  int likely_textured_repeat = 0;
  unsigned textured_repeat_score = 0;
  unsigned textured_repeat_spans = 0;
#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
  if (early_native && mode != MODE_GRAY && !likely_palette && !likely_blocks) {
#ifdef QLIC_STREAM_TRACE
    double textured_probe_begin = qlic_candidate_trace_now();
#endif
    likely_textured_repeat = probe_textured_spans(im, &textured_repeat_score,
                                                  &textured_repeat_spans);
#ifdef QLIC_STREAM_TRACE
    qlic_candidate_trace_phase("textured-repeat-probe", textured_probe_begin);
    if (qlic_candidate_trace_enabled())
      fprintf(
          stderr, "candidate-textured-repeat useful=%d score=%u eligible=%u\n",
          likely_textured_repeat, textured_repeat_score, textured_repeat_spans);
#endif
  }
#else
  (void)textured_repeat_score;
  (void)textured_repeat_spans;
#endif
#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
  if (likely_palette && mode != MODE_GRAY &&
      pixels > REPEAT_LZMS_PALETTE_MAX_PIXELS &&
      pixels <= REPEAT_LZMS_EXACT_PALETTE_MAX_PIXELS) {
    size_t incumbent = cand_size(best);
    if (!try_exact_palette_repeat_lzms(best, im, palette_probe_count))
      return 0;
    if (cand_size(best) < incumbent)
      return 1;
  }
#endif
  /* Large native images can be nonstationary across long vertical spans.
     Reuse the existing independent-QST band syntax only when the completed
     incumbent is native, and require a material completed-file win.  The
     byte decision remains image-content based; paths and corpus labels never
     participate. */
  if (!try_native_tile_bands(best, im, channels, stream_search, pixels))
    return 0;
  int retry_alpha_palette = early_native && mode == MODE_RGBA &&
                            pixels <= 2000000u && best->mode == MODE_NATIVE &&
                            sparse_endpoint_alpha(im, pixels);
  if (retry_alpha_palette) {
    if (!try_alpha_palette(best, im, codecs, codec_count))
      return 0;
    return 1;
  }
  /* Repeated blocks and an exact low-cardinality palette are independent
     evidence that a byte-oriented LZMS candidate can repay its encode cost. */
  if (early_native && (likely_blocks || likely_palette)) {
#ifdef QLIC_STREAM_TRACE
    double repeat_begin = qlic_candidate_trace_now();
#endif
    if (!try_repeat_lzms(best, im, mode, pixels,
                         likely_palette ? palette_probe_count : 0u))
      return 0;
#ifdef QLIC_STREAM_TRACE
    qlic_candidate_trace_phase("repeat-retry", repeat_begin);
    if (qlic_candidate_trace_enabled())
      fprintf(stderr, "candidate-state name=repeat-retry mode=%d bytes=%zu\n",
              best->compressed ? best->mode : -1, cand_size(best));
#endif
    return 1;
  }
  if (early_native && likely_textured_repeat) {
#ifdef QLIC_STREAM_TRACE
    double textured_repeat_begin = qlic_candidate_trace_now();
#endif
    if (!try_textured_repeat_lzms(best, im, mode))
      return 0;
#ifdef QLIC_STREAM_TRACE
    qlic_candidate_trace_phase("textured-repeat-retry", textured_repeat_begin);
    if (qlic_candidate_trace_enabled())
      fprintf(stderr,
              "candidate-state name=textured-repeat-retry mode=%d bytes=%zu\n",
              best->compressed ? best->mode : -1, cand_size(best));
#endif
    return 1;
  }
  if (early_native
#ifdef QLIC_STREAM_TRACE
      && !qlic_force_deep_trace_enabled()
#endif
  ) {
    if (best->compressed_size > raw_bytes) {
      /* incompressible input still needs a bounded size fallback */
      Buf samples = {0};
      if (!mk_samp(im, mode, TRANSFORM_IDENTITY, &samples) ||
          !try_store_owned(best, &samples, mode, TRANSFORM_IDENTITY_RAW, 0,
                           0)) {
        buf_free(&samples);
        return 0;
      }
      buf_free(&samples);
    }
    return 1;
  }
  if (!noisy && pixels > 16000000u &&
      !try_tile_height(best, im, channels, stream_search, 2048u, SIZE_MAX))
    return 0;
  if (large_quick) {
    uint32_t tile_h = im->height < 1024u ? im->height : 1024u;
    if (!noisy && !best->compressed &&
        !try_rtt_height(best, im, channels, 1, stream_search, tile_h))
      return 0;
  } else if (pixels <= 4096u) {
    if (!try_rtt_height(best, im, channels, 7, 7, im->height))
      return 0;
  }
  int sparse_large_palette =
      likely_blocks && has_sparse_large_palette(im, pixels);
  /* Tiny inputs are low-cost; larger block searches need the block probe. */
  if (pixels <= 64000000u &&
      (pixels < 65536u || (likely_blocks && !sparse_large_palette))) {
#ifdef QLIC_STREAM_TRACE
    double block_search_begin = qlic_candidate_trace_now();
#endif
    int block_ok = tried_blocks_base
                       ? try_blocks_ex(best, im, channels, stream_search, 1)
                       : try_blocks(best, im, channels, stream_search);
#ifdef QLIC_STREAM_TRACE
    qlic_candidate_trace_phase("block-search", block_search_begin);
    if (qlic_candidate_trace_enabled())
      fprintf(stderr, "candidate-state name=block-search mode=%d bytes=%zu\n",
              best->compressed ? best->mode : -1, cand_size(best));
#endif
    if (!block_ok)
      return 0;
  }

#ifdef QLIC_STREAM_TRACE
  double gmodel_begin = qlic_candidate_trace_now();
#endif
  if (!try_gmodel(best, im, mode, codecs, codec_count))
    return 0;
#ifdef QLIC_STREAM_TRACE
  qlic_candidate_trace_phase("gmodel", gmodel_begin);
#endif

  if (quick_special && !separable_checked) {
    int matched = 0;
    if (!try_separable(best, im, mode, codecs, codec_count, &matched))
      return 0;
  }

  {
    uint32_t limit = 256u;
    if (mode != MODE_GRAY && pixels <= 2000000u)
      limit = 65536u;
    Buf palette = {0};
    Buf indices = {0};
    Buf index_runs = {0};
    Buf pred_runs = {0};
    int index_bits = 0;
    uint32_t palette_count = 0;
    int palette_ok = 1;
    int palette_done = 0;
#ifdef QLIC_STREAM_TRACE
    double palette_build_begin = qlic_candidate_trace_now();
#endif
    int pr = large_quick && (!likely_palette || !probe_palette(im, limit))
                 ? 0
                 : mk_pal(im, limit, &palette, &indices, &index_runs,
                          &pred_runs, &index_bits, &palette_count, NULL);
#ifdef QLIC_STREAM_TRACE
    qlic_candidate_trace_phase("palette-build", palette_build_begin);
    double palette_trials_begin = qlic_candidate_trace_now();
#endif
    if (pr < 0)
      palette_ok = 0;
    if (palette_ok && pr > 0 && palette_count > 1) {
#ifdef QLIC_STREAM_TRACE
      if (qlic_candidate_trace_enabled())
        fprintf(stderr,
                "candidate-state name=palette-buffers colors=%u indices=%zu "
                "index-runs=%zu pred-runs=%zu\n",
                palette_count, indices.size, index_runs.size, pred_runs.size);
#endif
      int quick_palette = quick_special && palette_count <= 256u;
      int large_palette =
          palette_count > 256u && palette_count <= 65536u && pixels <= 2000000u;
      size_t row_bytes = row_pack(im->width, index_bits);
      int predictor_bpp = index_bits == 16 ? 2 : 1;
      size_t before_cpal = best->compressed ? best->compressed_size : SIZE_MAX;
      size_t palette_structure = indices.size;
      if (index_runs.size < palette_structure)
        palette_structure = index_runs.size;
      if (pred_runs.size < palette_structure)
        palette_structure = pred_runs.size;
      int compact_cpal =
          !compact_palette || before_cpal == SIZE_MAX ||
          (uint64_t)palette_structure * 2u <= (uint64_t)before_cpal * 7u;
      if (quick_palette && compact_cpal &&
          !try_cpal(best, &indices, &palette, TRANSFORM_IDENTITY_RAW,
                    index_bits, palette_count, im->width, im->height, codecs,
                    codec_count))
        palette_ok = 0;
      int cpal_won = palette_ok && best->compressed &&
                     best->mode == MODE_CPAL &&
                     best->compressed_size < before_cpal;
      if (palette_ok && compact_palette) {
        int ppal_candidate =
            mode == MODE_GRAY && palette_count > 16u &&
            pred_runs.size < indices.size / 2u &&
            (uint64_t)pred_runs.size * 2u <= (uint64_t)before_cpal * 9u;
        if (ppal_candidate) {
          if (!try_pay(best, &pred_runs, &palette, MODE_PPAL,
                       TRANSFORM_INDEX_RLE, index_bits, palette_count, codecs,
                       codec_count))
            palette_ok = 0;
        } else if (!cpal_won && mode == MODE_GRAY && palette_count >= 16u &&
                   palette_count <= 128u) {
          if (!try_pstream(best, &indices, row_bytes, im->height, predictor_bpp,
                           stream_search, &palette, index_bits, palette_count,
                           stream_codecs, 1))
            palette_ok = 0;
        }
        palette_done = best->compressed != NULL;
      }
      if (palette_ok && !palette_done && large_palette &&
          !try_cpal(best, &index_runs, &palette, TRANSFORM_CPAL_DELTA,
                    index_bits, palette_count, im->width, im->height, codecs,
                    codec_count))
        palette_ok = 0;
      /* Above 256 colors, runs are not always the smaller compressed
         representation. mk_pal already built row-packed indices, so let that
         existing CPAL syntax compete when its uncompressed structure is the
         smaller of the two. Completed bytes remain authoritative. */
      if (palette_ok && !palette_done && large_palette &&
          indices.size < index_runs.size &&
          !try_cpal(best, &indices, &palette, TRANSFORM_IDENTITY_RAW,
                    index_bits, palette_count, im->width, im->height, codecs,
                    codec_count))
        palette_ok = 0;
      if (palette_ok && !compact_palette && best->compressed &&
          best->mode == MODE_CPAL && best->compressed_size <= raw_bytes / 8u)
        palette_done = 1;
      if (palette_ok && !palette_done && !large_palette &&
          !try_rows(best, indices.data, row_bytes, im->height, predictor_bpp,
                    &palette, MODE_PALETTE, TRANSFORM_IDENTITY, index_bits,
                    palette_count, codecs, codec_count))
        palette_ok = 0;
      if (palette_ok && !palette_done && quick_palette &&
          !try_pay(best, &indices, &palette, MODE_PALETTE,
                   TRANSFORM_IDENTITY_RAW, index_bits, palette_count, codecs,
                   codec_count))
        palette_ok = 0;
      if (palette_ok && !palette_done && quick_palette &&
          !try_pstream(best, &indices, row_bytes, im->height, predictor_bpp,
                       stream_search, &palette, index_bits, palette_count,
                       stream_codecs, 1))
        palette_ok = 0;
      if (palette_ok && !palette_done && quick_palette &&
          !try_rle(best, &indices, &palette, MODE_PALETTE,
                   TRANSFORM_IDENTITY_RLE, index_bits, palette_count, codecs,
                   codec_count))
        palette_ok = 0;
      if (palette_ok && !palette_done && quick_palette &&
          !try_pay(best, &pred_runs, &palette, MODE_PPAL, TRANSFORM_INDEX_RLE,
                   index_bits, palette_count, codecs, codec_count))
        palette_ok = 0;
      if (palette_ok && !palette_done && (quick_palette || large_palette) &&
          !try_pay(best, &index_runs, &palette, MODE_PALETTE,
                   TRANSFORM_INDEX_RLE, index_bits, palette_count, codecs,
                   codec_count))
        palette_ok = 0;
#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
      if (palette_ok && large_palette && qlic_cpal_planar_enabled() &&
          !try_cpal_planar_lzms(best, &indices, NULL, &palette, im->width,
                                im->height, index_bits, palette_count))
        palette_ok = 0;
#endif
    }
    buf_free(&palette);
    buf_free(&indices);
    buf_free(&index_runs);
    buf_free(&pred_runs);
#ifdef QLIC_STREAM_TRACE
    qlic_candidate_trace_phase("palette-trials", palette_trials_begin);
#endif
    if (!palette_ok)
      return 0;
    if (palette_done) {
#ifdef QLIC_STREAM_TRACE
      qlic_candidate_trace_phase("encode-total", encode_begin);
#endif
      return 1;
    }
  }

  if (noisy || !large_quick || !best->compressed) {
    Buf samples = {0};
    int transform = mode == MODE_RGB || mode == MODE_RGBA ? TRANSFORM_GDELTA
                                                          : TRANSFORM_IDENTITY;
    size_t row_bytes = 0;
    if (!mk_samp(im, mode, transform, &samples)) {
      buf_free(&samples);
      return 0;
    }
    if (!mulok((size_t)im->width, (size_t)mbpp(mode), &row_bytes) ||
        !try_rows(best, samples.data, row_bytes, im->height, mbpp(mode), NULL,
                  mode, transform, 0, 0, codecs, codec_count)) {
      buf_free(&samples);
      return 0;
    }
    if ((quick_special || large_quick) &&
        (transform == TRANSFORM_IDENTITY || transform == TRANSFORM_GDELTA)) {
      int raw_transform =
          is_gd(transform) ? TRANSFORM_GDELTA_RAW : TRANSFORM_IDENTITY_RAW;
      if (!try_pay(best, &samples, NULL, mode, raw_transform, 0, 0, codecs,
                   codec_count)) {
        buf_free(&samples);
        return 0;
      }
    }
    buf_free(&samples);
  }

  if (!best->compressed) {
    set_err("no compression candidate was produced");
    return 0;
  }
  return 1;
}

static int enc_best(const Image *im, Candidate *best) {
  if (!enc_best_base(im, best))
    return 0;
#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
  if (qlic_tile_palette_enabled() && !try_tile_palette_lzms(best, im))
    return 0;
#endif
  return 1;
}

static int mk_file(const Image *im, const Candidate *cand, Buf *file) {
  if (!cand->compressed || !cand->compressed_size || !cand->payload_size) {
    set_err("no compression candidate was produced");
    return 0;
  }
  uint8_t header[QLIC_HEADER_SIZE];
  memset(header, 0, sizeof(header));
  memcpy(header, QLIC_MAGIC, QLIC_MAGIC_SIZE);
  wr32(header + 4, im->width);
  wr32(header + 8, im->height);
  header[12] = (uint8_t)cand->mode;
  header[13] = (uint8_t)cand->transform;
  header[14] = (uint8_t)cand->index_bits;
  header[15] = (uint8_t)(cand->codec | QLIC_CODEC_CRC);
  wr32(header + 16, cand->palette_count);
  wr64(header + 20, cand->payload_size);
  if (!buf_append(file, header, sizeof(header)) ||
      !buf_append(file, cand->palette, cand->palette_size) ||
      !buf_append(file, cand->compressed, cand->compressed_size)) {
    return 0;
  }
  /* metadata is covered too, not just the compressed body */
  return buf_u32le(file, stream_crc32(file->data, file->size));
}

int rd_head_limited(const uint8_t *data, size_t size, QlicHeader *h,
                    const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  if (!data || !h) {
    set_err("invalid decoder input");
    return 0;
  }
  if (!decode_limits_valid(limits))
    return 0;
  if ((uint64_t)size > limits->max_file_bytes) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: file bytes");
    return 0;
  }
  if (size < QLIC_HEADER_SIZE ||
      memcmp(data, QLIC_MAGIC, QLIC_MAGIC_SIZE) != 0) {
    set_err("not a QLIC file");
    return 0;
  }
  h->width = rd32(data + 4);
  h->height = rd32(data + 8);
  h->mode = data[12];
  h->transform = data[13];
  h->index_bits = data[14];
  uint8_t packed_codec = data[15];
  h->codec = packed_codec & ~QLIC_CODEC_CRC;
  h->palette_count = rd32(data + 16);
  h->payload_size = rd64(data + 20);
  if ((packed_codec & QLIC_CODEC_CRC) == 0) {
    set_err("corrupt file: missing container checksum");
    return 0;
  }
  if (h->width == 0 || h->height == 0) {
    set_err("corrupt file: invalid dimensions");
    return 0;
  }
  uint64_t pixels = (uint64_t)h->width * h->height;
  if (pixels > limits->max_pixels || pixels > SIZE_MAX / 4u) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED, "resource limit exceeded: pixels");
    return 0;
  }
  if (h->payload_size > limits->max_payload_bytes) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: decoded payload bytes");
    return 0;
  }
  if (h->mode < MODE_GRAY || h->mode > MODE_HDR_WIDE ||
      h->mode == MODE_SOURCE || h->mode == MODE_RESERVED) {
    set_err("corrupt file: invalid mode");
    return 0;
  }
  if (h->transform < TRANSFORM_IDENTITY ||
      h->transform > TRANSFORM_CPAL_PLANAR) {
    set_err("corrupt file: invalid transform");
    return 0;
  }
  if ((packed_codec & ~(QLIC_CODEC_CRC | 3)) ||
      (h->codec != CODEC_STORE && h->codec != CODEC_LZMS)) {
    set_err("corrupt file: invalid codec");
    return 0;
  }
  if (is_planar_med(h->transform) &&
      ((h->mode != MODE_RGB && h->mode != MODE_RGBA) || h->index_bits != 0 ||
       h->palette_count != 0 || h->codec != CODEC_LZMS)) {
    set_err("corrupt file: invalid planar MED header");
    return 0;
  }
  if (h->mode == MODE_NATIVE && (h->transform != TRANSFORM_IDENTITY ||
                                 h->index_bits != 0 || h->palette_count != 0)) {
    /* the actual native transform is inside QST1, the outer value must stay
     * identity */
    set_err("corrupt file: invalid native stream header");
    return 0;
  }
  if (h->mode == MODE_FILTERED &&
      (h->index_bits < MODE_GRAY || h->index_bits > MODE_RGBA ||
       h->palette_count != 0)) {
    set_err("corrupt file: invalid filtered stream header");
    return 0;
  }
  if (h->mode == MODE_PSTREAM &&
      !palette_count_ok(h->palette_count, h->index_bits)) {
    set_err("corrupt file: invalid palette stream header");
    return 0;
  }
  if (h->mode == MODE_PPAL &&
      (!palette_count_ok(h->palette_count, h->index_bits) ||
       h->transform != TRANSFORM_INDEX_RLE)) {
    set_err("corrupt file: invalid ppal header");
    return 0;
  }
  if (h->mode == MODE_CPAL &&
      (!palette_count_ok(h->palette_count, h->index_bits) ||
       (h->transform != TRANSFORM_IDENTITY_RAW &&
        h->transform != TRANSFORM_INDEX_RLE &&
        h->transform != TRANSFORM_CPAL_DELTA &&
        h->transform != TRANSFORM_CPAL_TILES &&
        h->transform != TRANSFORM_CPAL_PLANAR))) {
    set_err("corrupt file: invalid cpalette header");
    return 0;
  }
  if (is_cpal_planar(h->transform) &&
      (h->mode != MODE_CPAL || h->palette_count <= 256u ||
       h->index_bits != pal_bits(h->palette_count))) {
    set_err("corrupt file: invalid planar cpalette header");
    return 0;
  }
  if (h->mode == MODE_TILES &&
      ((h->index_bits != 1 && h->index_bits != 3 && h->index_bits != 4) ||
       h->palette_count == 0 || h->transform != TRANSFORM_IDENTITY)) {
    set_err("corrupt file: invalid tile stream header");
    return 0;
  }
  if (h->mode == MODE_TILE_MODEL &&
      ((h->index_bits != 1 && h->index_bits != 3 && h->index_bits != 4) ||
       h->palette_count == 0 || h->transform != TRANSFORM_IDENTITY ||
       h->codec != CODEC_STORE)) {
    set_err("corrupt file: invalid tile model header");
    return 0;
  }
  if (h->mode == MODE_GMODEL && (h->transform != TRANSFORM_IDENTITY ||
                                 h->index_bits != 0 || h->palette_count != 0)) {
    set_err("corrupt file: invalid gray-model header");
    return 0;
  }
  if (h->mode == MODE_ANIM && (h->transform != TRANSFORM_IDENTITY ||
                               h->index_bits != 0 || h->palette_count == 0)) {
    set_err("corrupt file: invalid animation header");
    return 0;
  }
  if (h->mode == MODE_NATIVE_WIDE || h->mode == MODE_HDR_WIDE) {
    uint32_t channels = h->palette_count;
    uint32_t bits = (uint32_t)h->index_bits;
    uint64_t storage = bits <= 16u ? 2u : 4u;
    uint32_t minimum_bits = h->mode == MODE_HDR_WIDE ? 8u : 9u;
    if (h->transform != TRANSFORM_IDENTITY || bits < minimum_bits ||
        bits > 24u || (channels != 1u && channels != 3u && channels != 4u) ||
        h->codec != CODEC_STORE) {
      set_err("corrupt file: invalid wide stream header");
      return 0;
    }
    uint64_t bytes_per_pixel = (uint64_t)channels * storage;
    if (pixels > SIZE_MAX / bytes_per_pixel ||
        pixels > limits->max_decoded_bytes / bytes_per_pixel) {
      set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                     "resource limit exceeded: decoded pixel bytes");
      return 0;
    }
  } else if (pixels > limits->max_decoded_bytes / 4u) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: decoded pixel bytes");
    return 0;
  }
  if (h->mode == MODE_ANIM) {
    uint64_t count = h->palette_count;
    uint64_t table_bytes = count * sizeof(AnimFrame);
    uint64_t frame_bytes = pixels * 4u;
    if (count > limits->max_frames) {
      set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                     "resource limit exceeded: animation frames");
      return 0;
    }
    if (table_bytes > limits->max_animation_bytes ||
        frame_bytes > (limits->max_animation_bytes - table_bytes) / count) {
      set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                     "resource limit exceeded: animation memory");
      return 0;
    }
  }
  if (h->mode == MODE_BLOCKS &&
      (h->transform != TRANSFORM_IDENTITY ||
       (h->index_bits != BLK_SIZE && h->index_bits != CF_SIZE) ||
       h->palette_count != 0 || h->codec != CODEC_STORE)) {
    set_err("corrupt file: invalid block stream header");
    return 0;
  }
  if (h->mode == MODE_PALETTE &&
      !palette_count_ok(h->palette_count, h->index_bits)) {
    set_err("corrupt file: invalid palette header");
    return 0;
  }
  if (!file_palette_mode(h->mode) && h->mode != MODE_CPAL &&
      h->mode != MODE_TILES && h->mode != MODE_TILE_MODEL &&
      h->mode != MODE_ANIM && h->mode != MODE_NATIVE_WIDE &&
      h->mode != MODE_HDR_WIDE && h->palette_count != 0) {
    set_err("corrupt file: unexpected palette data");
    return 0;
  }
  size_t palette_size = file_palette_size(h);
  size_t start;
  if (!addok(QLIC_HEADER_SIZE, palette_size, &start))
    return 0;
  if (size < QLIC_HEADER_SIZE + QLIC_FOOTER_SIZE) {
    set_err("corrupt file: missing integrity footer");
    return 0;
  }
  size_t body_size = size - QLIC_FOOTER_SIZE;
  uint32_t got = rd32(data + body_size);
  uint32_t want = stream_crc32(data, body_size);
  if (got != want) {
    set_err("corrupt file: container checksum mismatch");
    return 0;
  }
  if (start > body_size) {
    set_err("corrupt file: palette exceeds file size");
    return 0;
  }
  h->compressed_size = (uint64_t)(body_size - start);
  return 1;
}

int rd_head(const uint8_t *data, size_t size, QlicHeader *h) {
  return rd_head_limited(data, size, h, NULL);
}

static const uint8_t *read_rgb(uint8_t *p, const uint8_t *s, int transform,
                               int alpha) {
  if (is_gd(transform)) {
    uint8_t g = s[0];
    p[0] = (uint8_t)(g + s[1]);
    p[1] = g;
    p[2] = (uint8_t)(g + s[2]);
  } else if (is_rd(transform)) {
    uint8_t r = s[0];
    p[0] = r;
    p[1] = (uint8_t)(r + s[1]);
    p[2] = (uint8_t)(r + s[2]);
  } else if (is_bd(transform)) {
    uint8_t b = s[0];
    p[0] = (uint8_t)(b + s[1]);
    p[1] = (uint8_t)(b + s[2]);
    p[2] = b;
  } else {
    memcpy(p, s, 3);
  }
  p[3] = alpha ? s[3] : 255;
  return s + (alpha ? 4u : 3u);
}

static int samp_rgba(const Buf *samples, const QlicHeader *h,
                     const uint8_t *palette, Image *out) {
  size_t pixels, bytes;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4, &bytes))
    return 0;
  out->rgba = (uint8_t *)xmalloc(bytes);
  if (!out->rgba)
    return 0;
  out->width = h->width;
  out->height = h->height;

  if (h->mode == MODE_PALETTE) {
    if (!valid_index_bits(h->index_bits)) {
      image_free(out);
      set_err("corrupt file: invalid palette index bit width");
      return 0;
    }
    size_t row_bytes = row_pack(h->width, h->index_bits);
    size_t need;
    if (!mulok(row_bytes, (size_t)h->height, &need)) {
      image_free(out);
      return 0;
    }
    if (samples->size != need) {
      image_free(out);
      set_err("corrupt file: palette payload size mismatch");
      return 0;
    }
    for (uint32_t y = 0; y < h->height; ++y) {
      const uint8_t *row = samples->data + (size_t)y * row_bytes;
      for (uint32_t x = 0; x < h->width; ++x) {
        uint32_t idx = unpack_i(row, x, h->index_bits);
        if (idx >= h->palette_count) {
          image_free(out);
          set_err("corrupt file: palette index out of range");
          return 0;
        }
        memcpy(out->rgba + ((size_t)y * h->width + x) * 4u, palette + idx * 4u,
               4);
      }
    }
    return 1;
  }

  int bpp = mbpp(h->mode);
  if (bpp <= 0) {
    image_free(out);
    set_err("corrupt file: invalid mode");
    return 0;
  }
  size_t need;
  if (!mulok(pixels, (size_t)bpp, &need)) {
    image_free(out);
    return 0;
  }
  if (samples->size != need) {
    image_free(out);
    set_err("corrupt file: sample payload size mismatch");
    return 0;
  }
  const uint8_t *s = samples->data;
  if (h->mode == MODE_GRAY || h->mode == MODE_GRAYA) {
    int alpha = h->mode == MODE_GRAYA;
    for (size_t i = 0; i < pixels; ++i) {
      uint8_t *p = out->rgba + i * 4u;
      p[0] = p[1] = p[2] = s[0];
      p[3] = alpha ? s[1] : 255;
      s += alpha ? 2u : 1u;
    }
  } else {
    int alpha = h->mode == MODE_RGBA;
    for (size_t i = 0; i < pixels; ++i)
      s = read_rgb(out->rgba + i * 4u, s, h->transform, alpha);
  }
  return 1;
}

static int planar_med_rgba(Buf *residual, const QlicHeader *h, Image *out) {
  size_t pixels = 0;
  size_t payload_bytes = 0;
  size_t rgba_bytes = 0;
  int channels = mbpp(h->mode);
  if ((h->mode != MODE_RGB && h->mode != MODE_RGBA) ||
      !mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, (size_t)channels, &payload_bytes) ||
      residual->size != payload_bytes || !mulok(pixels, 4u, &rgba_bytes)) {
    set_err("corrupt file: invalid planar MED payload");
    return 0;
  }

  out->rgba = (uint8_t *)xmalloc(rgba_bytes);
  if (!out->rgba)
    return 0;
  out->width = h->width;
  out->height = h->height;
  uint8_t *blue = residual->data;
  uint8_t *red_delta = blue + pixels;
  uint8_t *green_delta = red_delta + pixels;
  uint8_t *alpha = channels == 4 ? green_delta + pixels : NULL;

  /* The first row is a prefix sum and the first column predicts from above.
     Keeping those edges outside the hot loop removes all per-sample boundary
     branches. Reconstruction and RGBA emission share one cache-friendly pass.
   */
  for (uint32_t x = 0; x < h->width; ++x) {
    size_t pos = x;
    if (x) {
      blue[pos] = (uint8_t)(blue[pos] + blue[pos - 1u]);
      red_delta[pos] = (uint8_t)(red_delta[pos] + red_delta[pos - 1u]);
      green_delta[pos] = (uint8_t)(green_delta[pos] + green_delta[pos - 1u]);
      if (alpha)
        alpha[pos] = (uint8_t)(alpha[pos] + alpha[pos - 1u]);
    }
    uint8_t *pixel = out->rgba + pos * 4u;
    pixel[0] = (uint8_t)(blue[pos] + red_delta[pos]);
    pixel[1] = (uint8_t)(blue[pos] + green_delta[pos]);
    pixel[2] = blue[pos];
    pixel[3] = alpha ? alpha[pos] : 255u;
  }
  for (uint32_t y = 1; y < h->height; ++y) {
    size_t row = (size_t)y * h->width;
    blue[row] = (uint8_t)(blue[row] + blue[row - h->width]);
    red_delta[row] = (uint8_t)(red_delta[row] + red_delta[row - h->width]);
    green_delta[row] =
        (uint8_t)(green_delta[row] + green_delta[row - h->width]);
    if (alpha)
      alpha[row] = (uint8_t)(alpha[row] + alpha[row - h->width]);
    uint8_t *first = out->rgba + row * 4u;
    first[0] = (uint8_t)(blue[row] + red_delta[row]);
    first[1] = (uint8_t)(blue[row] + green_delta[row]);
    first[2] = blue[row];
    first[3] = alpha ? alpha[row] : 255u;

    for (uint32_t x = 1; x < h->width; ++x) {
      size_t pos = row + x;
      uint8_t b = planar_med_step(blue, pos, h->width);
      uint8_t rd = planar_med_step(red_delta, pos, h->width);
      uint8_t gd = planar_med_step(green_delta, pos, h->width);
      uint8_t a = alpha ? planar_med_step(alpha, pos, h->width) : 255u;
      uint8_t *pixel = out->rgba + pos * 4u;
      pixel[0] = (uint8_t)(b + rd);
      pixel[1] = (uint8_t)(b + gd);
      pixel[2] = b;
      pixel[3] = a;
    }
  }
  return 1;
}

static int samp_size(const QlicHeader *h, size_t *out) {
  size_t pixels;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels))
    return 0;
  if (h->mode == MODE_PALETTE) {
    if (!valid_index_bits(h->index_bits)) {
      set_err("corrupt file: invalid palette index bit width");
      return 0;
    }
    return mulok(row_pack(h->width, h->index_bits), (size_t)h->height, out);
  }
  int bpp = mbpp(h->mode);
  if (bpp <= 0) {
    set_err("corrupt file: invalid sample mode");
    return 0;
  }
  return mulok(pixels, (size_t)bpp, out);
}

static int dec_irun(const Buf *runs, const QlicHeader *h,
                    const uint8_t *palette, Image *out) {
  size_t pixels, bytes;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4, &bytes))
    return 0;
  out->rgba = (uint8_t *)xmalloc(bytes);
  if (!out->rgba)
    return 0;
  out->width = h->width;
  out->height = h->height;

  size_t pos = 0;
  size_t pixel = 0;
  while (pos < runs->size) {
    size_t runm1 = 0;
    size_t idx = 0;
    if (!read_varint(runs->data, runs->size, &pos, &runm1) ||
        !read_varint(runs->data, runs->size, &pos, &idx)) {
      image_free(out);
      return 0;
    }
    if (runm1 == SIZE_MAX) {
      image_free(out);
      set_err("corrupt file: palette run overflow");
      return 0;
    }
    size_t run = runm1 + 1u;
    if (idx >= h->palette_count || run > pixels - pixel) {
      image_free(out);
      set_err("corrupt file: bad palette run");
      return 0;
    }
    const uint8_t *c = palette + idx * 4u;
    for (size_t i = 0; i < run; ++i) {
      memcpy(out->rgba + (pixel + i) * 4u, c, 4);
    }
    pixel += run;
  }
  if (pixel != pixels) {
    image_free(out);
    set_err("corrupt file: palette run payload size mismatch");
    return 0;
  }
  return 1;
}

static int dec_sep(const Buf *payload, const QlicHeader *h, Image *out) {
  int base_mode = h->index_bits;
  int bpp = mbpp(base_mode);
  if (bpp <= 0) {
    set_err("corrupt file: invalid separable base mode");
    return 0;
  }
  size_t sbpp = (size_t)bpp;
  size_t row_bytes;
  if (!mulok((size_t)h->width, sbpp, &row_bytes))
    return 0;
  size_t expected;
  if (!addok(row_bytes, (size_t)(h->height - 1u) * sbpp, &expected))
    return 0;
  if (payload->size != expected) {
    set_err("corrupt file: separable payload size mismatch");
    return 0;
  }
  Buf raw = {0};
  const uint8_t *table = payload->data;
  if (h->transform == TRANSFORM_SEPARABLE_DELTA) {
    if (!buf_reserve(&raw, expected))
      return 0;
    raw.size = expected;
    uint8_t *row0 = raw.data;
    const uint8_t *delta = payload->data;
    if (!row0) {
      buf_free(&raw);
      return 0;
    }
    memcpy(row0, delta, sbpp);
    delta += sbpp;
    for (uint32_t x = 1; x < h->width; ++x) {
      for (int ch = 0; ch < bpp; ++ch) {
        size_t c = (size_t)ch;
        row0[(size_t)x * sbpp + c] =
            (uint8_t)(row0[(size_t)(x - 1u) * sbpp + c] + *delta++);
      }
    }
    uint8_t *cols = raw.data + row_bytes;
    const uint8_t *prev = row0;
    for (uint32_t y = 1; y < h->height; ++y) {
      uint8_t *cur = cols + (size_t)(y - 1u) * sbpp;
      for (int ch = 0; ch < bpp; ++ch) {
        size_t c = (size_t)ch;
        cur[c] = (uint8_t)(prev[c] + *delta++);
      }
      prev = cur;
    }
    table = raw.data;
  }
  Buf samples = {0};
  size_t sample_size;
  if (!mulok(row_bytes, (size_t)h->height, &sample_size) ||
      !buf_reserve(&samples, sample_size)) {
    buf_free(&samples);
    buf_free(&raw);
    return 0;
  }
  samples.size = sample_size;
  const uint8_t *row0 = table;
  const uint8_t *cols = table + row_bytes;
  for (uint32_t y = 0; y < h->height; ++y) {
    uint8_t *row = samples.data + (size_t)y * row_bytes;
    const uint8_t *col = y == 0 ? row0 : cols + (size_t)(y - 1u) * sbpp;
    for (uint32_t x = 0; x < h->width; ++x) {
      for (int ch = 0; ch < bpp; ++ch) {
        size_t c = (size_t)ch;
        row[(size_t)x * sbpp + c] =
            (uint8_t)(row0[(size_t)x * sbpp + c] + col[c] - row0[c]);
      }
    }
  }
  QlicHeader sh = *h;
  sh.mode = base_mode;
  sh.transform = TRANSFORM_IDENTITY_RAW;
  int ok = samp_rgba(&samples, &sh, NULL, out);
  buf_free(&samples);
  buf_free(&raw);
  return ok;
}

static int dec_stream(const Buf *payload, const QlicHeader *h, Image *out) {
  uint8_t *pix = NULL;
  uint32_t w = 0, hh = 0;
  int ch = 0;
  int err = stream_decode_trusted_expected_rgba(
      payload->data, payload->size, h->width, h->height, 0, &pix, &w, &hh, &ch);
  if (err != STREAM_OK) {
    set_err_status(stream_failure_status(err, 1),
                   "native stream decode failed: %s", stream_strerror(err));
    return 0;
  }
  if (w != h->width || hh != h->height || ch != 4) {
    stream_free(pix);
    set_err("corrupt file: native stream dimensions or channel count mismatch");
    return 0;
  }
  out->rgba = pix;
  out->width = w;
  out->height = hh;
  return 1;
}

typedef struct {
  const uint8_t *data;
  size_t size;
  uint32_t y;
  uint32_t h;
  uint32_t w;
  int channels;
  unsigned inner_threads;
  Image *out;
  int err;
} TileDecTask;

static void tile_copy_rgba(TileDecTask *t, const uint8_t *pix) {
  uint8_t *dst0 = t->out->rgba + (size_t)t->y * t->w * 4u;
  if (t->channels == 4) {
    for (uint32_t y = 0; y < t->h; ++y) {
      memcpy(dst0 + (size_t)y * t->w * 4u, pix + (size_t)y * t->w * 4u,
             (size_t)t->w * 4u);
    }
  } else if (t->channels == 3) {
    for (uint32_t y = 0; y < t->h; ++y) {
      const uint8_t *s = pix + (size_t)y * t->w * 3u;
      uint8_t *d = dst0 + (size_t)y * t->w * 4u;
      for (uint32_t x = 0; x < t->w; ++x) {
        d[x * 4u + 0] = s[x * 3u + 0];
        d[x * 4u + 1] = s[x * 3u + 1];
        d[x * 4u + 2] = s[x * 3u + 2];
        d[x * 4u + 3] = 255;
      }
    }
  } else {
    for (uint32_t y = 0; y < t->h; ++y) {
      const uint8_t *s = pix + (size_t)y * t->w;
      uint8_t *d = dst0 + (size_t)y * t->w * 4u;
      for (uint32_t x = 0; x < t->w; ++x) {
        uint8_t v = s[x];
        d[x * 4u + 0] = v;
        d[x * 4u + 1] = v;
        d[x * 4u + 2] = v;
        d[x * 4u + 3] = 255;
      }
    }
  }
}

static void tile_dec_one(TileDecTask *t) {
  uint8_t *pix = NULL;
  uint32_t w = 0, h = 0;
  int ch = 0;
  int err = stream_decode_trusted_expected_threads(
      t->data, t->size, t->inner_threads, t->w, t->h, t->channels, &pix, &w, &h,
      &ch);
  if (err != STREAM_OK) {
    t->err = err;
    return;
  }
  if (w != t->w || h != t->h || ch != t->channels) {
    stream_free(pix);
    t->err = STREAM_E_CORRUPT;
    return;
  }
  tile_copy_rgba(t, pix);
  stream_free(pix);
  t->err = STREAM_OK;
}

static void tile_dec_item(void *context, unsigned index) {
  tile_dec_one(&((TileDecTask *)context)[index]);
}

static int dec_tile(const Buf *payload, const QlicHeader *h, Image *out,
                    const QlicDecodeLimits *limits) {
  if (payload->size < 4u) {
    set_err("corrupt file: truncated tile stream payload");
    return 0;
  }
  uint32_t count = rd32(payload->data);
  uint32_t tile_h = h->palette_count;
  uint32_t want = 1u + (h->height - 1u) / tile_h;
  if ((uint64_t)count > limits->max_chunks) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: tile stream chunks");
    return 0;
  }
  if (count == 0 || count != want || count > 65536u || payload->size < 4u ||
      (size_t)count > (payload->size - 4u) / 4u) {
    set_err("corrupt file: invalid tile stream table");
    return 0;
  }
  size_t table = 4u + (size_t)count * 4u;
  size_t off = table;
  TileDecTask *tasks = (TileDecTask *)calloc(count, sizeof(TileDecTask));
  if (!tasks) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t size = rd32(payload->data + 4u + (size_t)i * 4u);
    if ((size_t)size > payload->size - off) {
      free(tasks);
      set_err("corrupt file: tile stream chunk exceeds payload");
      return 0;
    }
    uint32_t y0 = i * tile_h;
    tasks[i].data = payload->data + off;
    tasks[i].size = size;
    tasks[i].y = y0;
    tasks[i].h = h->height - y0 < tile_h ? h->height - y0 : tile_h;
    tasks[i].w = h->width;
    tasks[i].channels = h->index_bits;
    tasks[i].out = out;
    tasks[i].err = STREAM_E_CORRUPT;
    off += size;
  }
  if (off != payload->size) {
    free(tasks);
    set_err("corrupt file: trailing tile stream data");
    return 0;
  }
  size_t pixels = 0, bytes = 0;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes)) {
    free(tasks);
    return 0;
  }
  out->rgba = (uint8_t *)xmalloc(bytes);
  if (!out->rgba) {
    free(tasks);
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  unsigned threads = tile_band_worker_count(h->height, tile_h, count,
                                            g_threads ? g_threads : 1u);
  unsigned budget = g_threads ? g_threads : 1u;
  unsigned inner = budget / threads;
  unsigned extra = budget % threads;
  /* nested parallel work shares one budget to avoid oversubscribing the machine
   */
  for (uint32_t i = 0; i < count; ++i)
    tasks[i].inner_threads = inner + (i < extra);
  if (threads <= 1) {
    for (uint32_t i = 0; i < count; ++i)
      tile_dec_one(&tasks[i]);
  } else {
    qlic_parallel_for(count, threads, tile_dec_item, tasks);
  }
  int ok = 1;
  for (uint32_t i = 0; i < count; ++i) {
    if (tasks[i].err != STREAM_OK) {
      set_err_status(stream_failure_status(tasks[i].err, 1),
                     "tile stream decode failed: %s",
                     stream_strerror(tasks[i].err));
      ok = 0;
      break;
    }
  }
  free(tasks);
  if (!ok)
    image_free(out);
  return ok;
}

typedef struct {
  const uint8_t *data;
  size_t size;
  uint8_t model;
  uint32_t y;
  uint32_t h;
  uint32_t w;
  int channels;
  unsigned inner_threads;
  Image *out;
  int err;
} RTTDecTask;

static void rtt_copy_rgba(RTTDecTask *t, const uint8_t *pix) {
  uint8_t *dst0 = t->out->rgba + (size_t)t->y * t->w * 4u;
  if (t->channels == 4) {
    for (uint32_t y = 0; y < t->h; ++y) {
      memcpy(dst0 + (size_t)y * t->w * 4u, pix + (size_t)y * t->w * 4u,
             (size_t)t->w * 4u);
    }
  } else if (t->channels == 3) {
    for (uint32_t y = 0; y < t->h; ++y) {
      const uint8_t *s = pix + (size_t)y * t->w * 3u;
      uint8_t *d = dst0 + (size_t)y * t->w * 4u;
      for (uint32_t x = 0; x < t->w; ++x) {
        d[x * 4u + 0] = s[x * 3u + 0];
        d[x * 4u + 1] = s[x * 3u + 1];
        d[x * 4u + 2] = s[x * 3u + 2];
        d[x * 4u + 3] = 255;
      }
    }
  } else {
    for (uint32_t y = 0; y < t->h; ++y) {
      const uint8_t *s = pix + (size_t)y * t->w;
      uint8_t *d = dst0 + (size_t)y * t->w * 4u;
      for (uint32_t x = 0; x < t->w; ++x) {
        uint8_t v = s[x];
        d[x * 4u + 0] = v;
        d[x * 4u + 1] = v;
        d[x * 4u + 2] = v;
        d[x * 4u + 3] = 255;
      }
    }
  }
}

static int rtt_recon(uint8_t *pix, uint32_t w, uint32_t h, int ch, int model,
                     const uint8_t *par) {
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      for (int c = 0; c < ch; ++c) {
        size_t i = ((size_t)y * (size_t)w + (size_t)x) * (size_t)ch + (size_t)c;
        int pr = model == RTT_PLANAR
                     ? rtt_pred_planar(par, w, h, x, y, c)
                     : (model == RTT_RULE
                            ? rtt_rule_predict(pix, w, ch, x, y, c, par)
                            : rtt_pred_causal(pix, w, ch, x, y, c, model));
        pix[i] = (uint8_t)(pix[i] + pr);
      }
    }
  }
  return 1;
}

static void rtt_dec_one(RTTDecTask *t) {
  if (t->model > RTT_MAX_MODEL) {
    t->err = STREAM_E_CORRUPT;
    return;
  }
  if (t->model == RTT_FILT) {
    size_t row_bytes = 0;
    if (!mulok((size_t)t->w, (size_t)t->channels, &row_bytes) ||
        row_bytes + 1u > UINT32_MAX) {
      t->err = STREAM_E_CORRUPT;
      return;
    }
    uint8_t *pix = NULL;
    uint32_t w = 0, h = 0;
    int ch = 0;
    int err = stream_decode_trusted_expected_threads(
        t->data, t->size, t->inner_threads, (uint32_t)(row_bytes + 1u), t->h, 1,
        &pix, &w, &h, &ch);
    if (err != STREAM_OK) {
      t->err = err;
      return;
    }
    if (w != (uint32_t)(row_bytes + 1u) || h != t->h || ch != 1) {
      stream_free(pix);
      t->err = STREAM_E_CORRUPT;
      return;
    }
    Buf samples = {0};
    size_t payload_size;
    if (!mulok((size_t)w, (size_t)h, &payload_size) ||
        !unf_rows(pix, payload_size, row_bytes, t->h, t->channels, &samples)) {
      stream_free(pix);
      buf_free(&samples);
      t->err = STREAM_E_CORRUPT;
      return;
    }
    stream_free(pix);
    rtt_copy_rgba(t, samples.data);
    buf_free(&samples);
    t->err = STREAM_OK;
    return;
  }

  const uint8_t *par = NULL;
  const uint8_t *zdata = t->data;
  size_t zsize = t->size;
  if (t->model == RTT_PLANAR || t->model == RTT_RULE) {
    size_t parn = t->model == RTT_PLANAR ? (size_t)t->channels * 3u : 4u;
    if (zsize <= parn) {
      t->err = STREAM_E_CORRUPT;
      return;
    }
    par = zdata;
    zdata += parn;
    zsize -= parn;
    if (t->model == RTT_RULE && par[1] <= par[0]) {
      t->err = STREAM_E_CORRUPT;
      return;
    }
  }

  uint8_t *pix = NULL;
  uint32_t w = 0, h = 0;
  int ch = 0;
  int err = stream_decode_trusted_expected_threads(
      zdata, zsize, t->inner_threads, t->w, t->h, t->channels, &pix, &w, &h,
      &ch);
  if (err != STREAM_OK) {
    t->err = err;
    return;
  }
  if (w != t->w || h != t->h || ch != t->channels) {
    stream_free(pix);
    t->err = STREAM_E_CORRUPT;
    return;
  }
  if (t->model != RTT_RAW && !rtt_recon(pix, w, h, ch, t->model, par)) {
    stream_free(pix);
    t->err = STREAM_E_CORRUPT;
    return;
  }
  rtt_copy_rgba(t, pix);
  stream_free(pix);
  t->err = STREAM_OK;
}

static void rtt_dec_item(void *context, unsigned index) {
  rtt_dec_one(&((RTTDecTask *)context)[index]);
}

static int dec_rtt(const Buf *payload, const QlicHeader *h, Image *out) {
  if (payload->size < 4u) {
    set_err("corrupt file: truncated tile model payload");
    return 0;
  }
  uint32_t count = rd32(payload->data);
  uint32_t tile_h = h->palette_count;
  uint32_t want = 1u + (h->height - 1u) / tile_h;
  if (count == 0 || count != want || count > 65536u ||
      (size_t)count > (payload->size - 4u) / 5u) {
    set_err("corrupt file: invalid tile model table");
    return 0;
  }
  size_t table = 4u + (size_t)count * 5u;
  size_t off = table;
  RTTDecTask *tasks = (RTTDecTask *)calloc(count, sizeof(RTTDecTask));
  if (!tasks) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i) {
    size_t ent = 4u + (size_t)i * 5u;
    uint8_t model = payload->data[ent];
    uint32_t size = rd32(payload->data + ent + 1u);
    if (model > RTT_MAX_MODEL || (size_t)size > payload->size - off) {
      free(tasks);
      set_err("corrupt file: tile model chunk exceeds payload");
      return 0;
    }
    uint32_t y0 = i * tile_h;
    tasks[i].data = payload->data + off;
    tasks[i].size = size;
    tasks[i].model = model;
    tasks[i].y = y0;
    tasks[i].h = h->height - y0 < tile_h ? h->height - y0 : tile_h;
    tasks[i].w = h->width;
    tasks[i].channels = h->index_bits;
    tasks[i].out = out;
    tasks[i].err = STREAM_E_CORRUPT;
    off += size;
  }
  if (off != payload->size) {
    free(tasks);
    set_err("corrupt file: trailing tile model data");
    return 0;
  }
  size_t pixels = 0, bytes = 0;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes)) {
    free(tasks);
    return 0;
  }
  out->rgba = (uint8_t *)xmalloc(bytes);
  if (!out->rgba) {
    free(tasks);
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  unsigned threads = g_threads ? g_threads : 1u;
  if (threads > count)
    threads = count;
  unsigned budget = g_threads ? g_threads : 1u;
  unsigned inner = budget / threads;
  unsigned extra = budget % threads;
  for (uint32_t i = 0; i < count; ++i)
    tasks[i].inner_threads = inner + (i < extra);
  if (threads <= 1) {
    for (uint32_t i = 0; i < count; ++i)
      rtt_dec_one(&tasks[i]);
  } else {
    qlic_parallel_for(count, threads, rtt_dec_item, tasks);
  }
  int ok = 1;
  for (uint32_t i = 0; i < count; ++i) {
    if (tasks[i].err != STREAM_OK) {
      set_err_status(stream_failure_status(tasks[i].err, 1),
                     "tile model decode failed: %s",
                     stream_strerror(tasks[i].err));
      ok = 0;
      break;
    }
  }
  free(tasks);
  if (!ok)
    image_free(out);
  return ok;
}

static int dec_gmodel(const Buf *payload, const QlicHeader *h, Image *out) {
  if (payload->size < 4u) {
    set_err("corrupt file: truncated gray-model payload");
    return 0;
  }
  int p = payload->data[0];
  int bl = payload->data[1];
  int a = payload->data[2];
  int b = payload->data[3];
  if (!((p == 8 || p == 16 || p == 32) && (bl == 0 || bl == 8 || bl == 16))) {
    set_err("corrupt file: invalid gray-model parameters");
    return 0;
  }
  size_t pixels = 0;
  size_t bytes = 0;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  size_t pn = (size_t)p * (size_t)p;
  uint32_t bw = 0;
  uint32_t bh = 0;
  size_t blocks = 0;
  if (bl > 0) {
    bw = (h->width + (uint32_t)bl - 1u) / (uint32_t)bl;
    bh = (h->height + (uint32_t)bl - 1u) / (uint32_t)bl;
    if (!mulok((size_t)bw, (size_t)bh, &blocks))
      return 0;
  }
  size_t need = 0;
  if (!addok(4u, pn, &need) || !addok(need, blocks, &need) ||
      !addok(need, pixels, &need))
    return 0;
  if (payload->size != need) {
    set_err("corrupt file: gray-model payload size mismatch");
    return 0;
  }
  const uint8_t *phase = payload->data + 4u;
  const uint8_t *bt = phase + pn;
  const uint8_t *res = bt + blocks;
  out->rgba = (uint8_t *)xmalloc(bytes);
  if (!out->rgba)
    return 0;
  out->width = h->width;
  out->height = h->height;
  if (bl > 0) {
    size_t ri = 0;
    for (uint32_t by = 0; by < bh; ++by) {
      for (uint32_t bx = 0; bx < bw; ++bx) {
        uint8_t bv = bt[(size_t)by * bw + bx];
        uint32_t y1 = by * (uint32_t)bl;
        uint32_t y2 =
            y1 + (uint32_t)bl < h->height ? y1 + (uint32_t)bl : h->height;
        uint32_t x1 = bx * (uint32_t)bl;
        uint32_t x2 =
            x1 + (uint32_t)bl < h->width ? x1 + (uint32_t)bl : h->width;
        for (uint32_t y = y1; y < y2; ++y) {
          uint32_t byv = (uint32_t)b * y;
          uint32_t py = (uint32_t)((int)(y % (uint32_t)p) * p);
          for (uint32_t x = x1; x < x2; ++x) {
            uint8_t v =
                (uint8_t)((uint32_t)a * x + byv +
                          phase[py + (x % (uint32_t)p)] + bv + res[ri++]);
            uint8_t *d = out->rgba + ((size_t)y * h->width + x) * 4u;
            d[0] = v;
            d[1] = v;
            d[2] = v;
            d[3] = 255;
          }
        }
      }
    }
    if (ri != pixels) {
      image_free(out);
      set_err("corrupt file: gray-model residual mismatch");
      return 0;
    }
  } else {
    for (uint32_t y = 0; y < h->height; ++y) {
      uint32_t byv = (uint32_t)b * y;
      uint32_t py = (uint32_t)((int)(y % (uint32_t)p) * p);
      for (uint32_t x = 0; x < h->width; ++x) {
        size_t i = (size_t)y * h->width + x;
        uint8_t v = (uint8_t)((uint32_t)a * x + byv +
                              phase[py + (x % (uint32_t)p)] + res[i]);
        uint8_t *d = out->rgba + i * 4u;
        d[0] = v;
        d[1] = v;
        d[2] = v;
        d[3] = 255;
      }
    }
  }
  return 1;
}

static int dec_filtered(const Buf *payload, const QlicHeader *h, Image *out) {
  int base_mode = h->index_bits;
  int bpp = mbpp(base_mode);
  if (bpp <= 0) {
    set_err("corrupt file: invalid filtered stream base mode");
    return 0;
  }
  size_t row_bytes = 0;
  if (!mulok((size_t)h->width, (size_t)bpp, &row_bytes))
    return 0;
  if (row_bytes + 1u > UINT32_MAX) {
    set_err("corrupt file: filtered stream row is too wide");
    return 0;
  }
  uint8_t *pix = NULL;
  uint32_t zw = 0, zh = 0;
  int zch = 0;
  int err = stream_decode_trusted_expected(payload->data, payload->size,
                                           (uint32_t)(row_bytes + 1u),
                                           h->height, 1, &pix, &zw, &zh, &zch);
  if (err != STREAM_OK) {
    set_err_status(stream_failure_status(err, 1),
                   "filtered stream decode failed: %s", stream_strerror(err));
    return 0;
  }
  if (zw != (uint32_t)(row_bytes + 1u) || zh != h->height || zch != 1) {
    stream_free(pix);
    set_err("corrupt file: filtered stream dimensions mismatch");
    return 0;
  }
  Buf samples = {0};
  size_t payload_size;
  int ok = mulok((size_t)zw, (size_t)zh, &payload_size) &&
           unf_rows(pix, payload_size, row_bytes, h->height, bpp, &samples);
  stream_free(pix);
  if (!ok) {
    buf_free(&samples);
    return 0;
  }
  QlicHeader sh = *h;
  sh.mode = base_mode;
  sh.index_bits = 0;
  ok = samp_rgba(&samples, &sh, NULL, out);
  buf_free(&samples);
  return ok;
}

static int dec_pstream(const Buf *payload, const QlicHeader *h,
                       const uint8_t *palette, Image *out) {
  if (!valid_index_bits(h->index_bits) || h->palette_count == 0) {
    set_err("corrupt file: invalid palette stream index mode");
    return 0;
  }
  size_t row_bytes = row_pack(h->width, h->index_bits);
  size_t zrow = row_bytes;
  if (h->transform == TRANSFORM_IDENTITY)
    zrow = row_bytes + 1u;
  else if (h->transform != TRANSFORM_IDENTITY_RAW) {
    set_err("corrupt file: invalid palette stream transform");
    return 0;
  }
  if (zrow > UINT32_MAX) {
    set_err("corrupt file: palette stream row is too wide");
    return 0;
  }
  uint8_t *pix = NULL;
  uint32_t zw = 0, zh = 0;
  int zch = 0;
  int err = stream_decode_trusted_expected(payload->data, payload->size,
                                           (uint32_t)zrow, h->height, 1, &pix,
                                           &zw, &zh, &zch);
  if (err != STREAM_OK) {
    set_err_status(stream_failure_status(err, 1),
                   "palette stream decode failed: %s", stream_strerror(err));
    return 0;
  }
  if (zw != (uint32_t)zrow || zh != h->height || zch != 1) {
    stream_free(pix);
    set_err("corrupt file: palette stream dimensions mismatch");
    return 0;
  }
  Buf samples = {0};
  int ok = 0;
  if (h->transform == TRANSFORM_IDENTITY) {
    size_t payload_size;
    ok = mulok(zrow, (size_t)zh, &payload_size) &&
         unf_rows(pix, payload_size, row_bytes, h->height,
                  h->index_bits == 16 ? 2 : 1, &samples);
  } else {
    size_t bytes;
    if (mulok(row_bytes, (size_t)h->height, &bytes) &&
        buf_append(&samples, pix, bytes))
      ok = 1;
  }
  stream_free(pix);
  if (!ok) {
    buf_free(&samples);
    return 0;
  }
  QlicHeader ph = *h;
  ph.mode = MODE_PALETTE;
  ok = samp_rgba(&samples, &ph, palette, out);
  buf_free(&samples);
  return ok;
}

static int dec_ppal(const Buf *payload, const QlicHeader *h,
                    const uint8_t *palette, Image *out) {
  size_t pixels = 0;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels))
    return 0;
  uint16_t *ids = (uint16_t *)xmalloc(pixels * sizeof(uint16_t));
  if (!ids)
    return 0;
  size_t in = 0;
  size_t pos = 0;
  while (pos < pixels) {
    if (in >= payload->size) {
      free(ids);
      set_err("corrupt file: truncated ppal stream");
      return 0;
    }
    uint8_t op = payload->data[in++];
    if (op > 3) {
      free(ids);
      set_err("corrupt file: bad ppal opcode");
      return 0;
    }
    if (op == 3) {
      size_t v = 0;
      if (!read_varint(payload->data, payload->size, &in, &v) ||
          v >= h->palette_count) {
        free(ids);
        set_err("corrupt file: bad ppal literal");
        return 0;
      }
      ids[pos++] = (uint16_t)v;
      continue;
    }
    size_t runm1 = 0;
    if (!read_varint(payload->data, payload->size, &in, &runm1)) {
      free(ids);
      return 0;
    }
    if (runm1 == SIZE_MAX) {
      free(ids);
      set_err("corrupt file: ppal run overflow");
      return 0;
    }
    size_t run = runm1 + 1u;
    if (run > pixels - pos) {
      free(ids);
      set_err("corrupt file: ppal run exceeds image");
      return 0;
    }
    for (size_t i = 0; i < run; ++i) {
      size_t p = pos + i;
      uint32_t x = (uint32_t)(p % h->width);
      uint32_t y = (uint32_t)(p / h->width);
      if (op == 0) {
        if (x == 0) {
          free(ids);
          set_err("corrupt file: bad ppal left run");
          return 0;
        }
        ids[p] = ids[p - 1];
      } else if (op == 1) {
        if (y == 0) {
          free(ids);
          set_err("corrupt file: bad ppal up run");
          return 0;
        }
        ids[p] = ids[p - h->width];
      } else {
        if (x == 0 || y == 0) {
          free(ids);
          set_err("corrupt file: bad ppal diagonal run");
          return 0;
        }
        ids[p] = ids[p - h->width - 1u];
      }
    }
    pos += run;
  }
  if (in != payload->size) {
    free(ids);
    set_err("corrupt file: trailing ppal data");
    return 0;
  }
  Buf samples = {0};
  size_t row_bytes = row_pack(h->width, h->index_bits);
  size_t sample_bytes;
  if (!mulok(row_bytes, (size_t)h->height, &sample_bytes) ||
      !buf_reserve(&samples, sample_bytes)) {
    free(ids);
    return 0;
  }
  for (uint32_t y = 0; y < h->height; ++y) {
    if (!pack_row(ids + (size_t)y * h->width, h->width, h->index_bits,
                  &samples)) {
      free(ids);
      buf_free(&samples);
      return 0;
    }
  }
  free(ids);
  QlicHeader ph = *h;
  ph.mode = MODE_PALETTE;
  int ok = samp_rgba(&samples, &ph, palette, out);
  buf_free(&samples);
  return ok;
}

static int dec_cpal_tiles(const Buf *payload, const QlicHeader *h, Image *out) {
  enum { MIN_TILE_LOG = 3, MAX_TILE_LOG = 6 };
  size_t palette_size = (size_t)h->palette_count * 4u;
  size_t pixels = 0;
  size_t rgba_bytes = 0;
  if (payload->size < 1u + palette_size ||
      !mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &rgba_bytes)) {
    set_err("corrupt file: tile-palette payload size mismatch");
    return 0;
  }
  unsigned tile_log = payload->data[0];
  if (tile_log < MIN_TILE_LOG || tile_log > MAX_TILE_LOG) {
    set_err("corrupt file: invalid tile-palette size");
    return 0;
  }
  uint32_t tile_size = 1u << tile_log;
  size_t max_tile_pixels = (size_t)tile_size * tile_size;
  uint16_t *local_palette =
      (uint16_t *)xmalloc(max_tile_pixels * sizeof(uint16_t));
  out->rgba = (uint8_t *)xmalloc(rgba_bytes);
  if (!local_palette || !out->rgba) {
    free(local_palette);
    free(out->rgba);
    out->rgba = NULL;
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  const uint8_t *palette = payload->data + 1u;
  size_t position = 1u + palette_size;

  for (uint32_t y0 = 0; y0 < h->height; y0 += tile_size) {
    uint32_t tile_height =
        h->height - y0 < tile_size ? h->height - y0 : tile_size;
    for (uint32_t x0 = 0; x0 < h->width; x0 += tile_size) {
      uint32_t tile_width =
          h->width - x0 < tile_size ? h->width - x0 : tile_size;
      size_t tile_pixels = (size_t)tile_width * tile_height;
      size_t local_count_m1 = 0;
      if (!read_varint(payload->data, payload->size, &position,
                       &local_count_m1) ||
          local_count_m1 >= tile_pixels || local_count_m1 >= h->palette_count) {
        image_free(out);
        free(local_palette);
        set_err("corrupt file: invalid tile-local palette size");
        return 0;
      }
      size_t local_count = local_count_m1 + 1u;
      size_t previous = 0;
      for (size_t i = 0; i < local_count; ++i) {
        size_t gap = 0;
        if (!read_varint(payload->data, payload->size, &position, &gap) ||
            (i && (previous == SIZE_MAX || gap > SIZE_MAX - previous - 1u))) {
          image_free(out);
          free(local_palette);
          set_err("corrupt file: invalid tile-local palette gap");
          return 0;
        }
        size_t global_index = i ? previous + gap + 1u : gap;
        if (global_index >= h->palette_count) {
          image_free(out);
          free(local_palette);
          set_err("corrupt file: tile-local palette index out of range");
          return 0;
        }
        local_palette[i] = (uint16_t)global_index;
        previous = global_index;
      }

      int local_bits = pal_bits((uint32_t)local_count);
      size_t packed_bits = 0;
      if (!mulok(tile_pixels, (size_t)local_bits, &packed_bits) ||
          packed_bits > SIZE_MAX - 7u) {
        image_free(out);
        free(local_palette);
        return 0;
      }
      size_t packed_bytes = (packed_bits + 7u) >> 3;
      if (position > payload->size || packed_bytes > payload->size - position) {
        image_free(out);
        free(local_palette);
        set_err("corrupt file: truncated tile-local indices");
        return 0;
      }
      const uint8_t *packed = payload->data + position;
      size_t pixel_index = 0;
      for (uint32_t y = 0; y < tile_height; ++y) {
        uint8_t *output = out->rgba + ((size_t)(y0 + y) * h->width + x0) * 4u;
        for (uint32_t x = 0; x < tile_width; ++x, ++pixel_index) {
          uint32_t local_index =
              unpack_i(packed, (uint32_t)pixel_index, local_bits);
          if (local_index >= local_count) {
            image_free(out);
            free(local_palette);
            set_err("corrupt file: tile-local index out of range");
            return 0;
          }
          memcpy(output + (size_t)x * 4u,
                 palette + (size_t)local_palette[local_index] * 4u, 4u);
        }
      }
      position += packed_bytes;
    }
  }
  free(local_palette);
  if (position != payload->size) {
    image_free(out);
    set_err("corrupt file: trailing tile-palette data");
    return 0;
  }
  return 1;
}

static int dec_cpal(const Buf *payload, const QlicHeader *h, Image *out) {
  if (is_cpal_tiles(h->transform))
    return dec_cpal_tiles(payload, h, out);
  size_t palette_size = (size_t)h->palette_count * 4u;
  if (payload->size < palette_size) {
    set_err("corrupt file: cpalette payload size mismatch");
    return 0;
  }
  if (is_cpal_planar(h->transform)) {
    size_t pixels = 0;
    size_t index_bytes = 0;
    size_t expected = 0;
    size_t rgba_bytes = 0;
    if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
        !mulok(pixels, 2u, &index_bytes) ||
        !addok(1u, palette_size, &expected) ||
        !addok(expected, index_bytes, &expected) || payload->size != expected ||
        !mulok(pixels, 4u, &rgba_bytes)) {
      set_err("corrupt file: planar cpalette payload size mismatch");
      return 0;
    }
    unsigned layout = payload->data[0];
    if (layout >= CPAL_PLANAR_LAYOUT_COUNT) {
      set_err("corrupt file: invalid planar cpalette layout");
      return 0;
    }
    uint8_t *palette = (uint8_t *)xmalloc(palette_size);
    uint8_t *rgba = (uint8_t *)xmalloc(rgba_bytes);
    if (!palette || !rgba) {
      free(palette);
      free(rgba);
      return 0;
    }
    size_t position = 1u;
    for (unsigned channel = 0; channel < 4u; ++channel) {
      uint8_t previous = 0;
      for (uint32_t i = 0; i < h->palette_count; ++i) {
        uint8_t delta = payload->data[position++];
        uint8_t value = i ? (uint8_t)(previous + delta) : delta;
        palette[(size_t)i * 4u + channel] = value;
        previous = value;
      }
    }
    const uint8_t *indices = payload->data + position;
    const uint8_t *high =
        layout == CPAL_PLANAR_SPLIT16 ? indices + pixels : NULL;
    for (size_t i = 0; i < pixels; ++i) {
      uint32_t index = layout == CPAL_PLANAR_INTERLEAVED16
                           ? rd16(indices + i * 2u)
                           : (uint32_t)indices[i] | ((uint32_t)high[i] << 8);
      if (index >= h->palette_count) {
        free(palette);
        free(rgba);
        set_err("corrupt file: planar cpalette index out of range");
        return 0;
      }
      memcpy(rgba + i * 4u, palette + (size_t)index * 4u, 4u);
    }
    free(palette);
    out->rgba = rgba;
    out->width = h->width;
    out->height = h->height;
    return 1;
  }
  if (h->transform == TRANSFORM_INDEX_RLE) {
    Buf runs = {0};
    runs.data = payload->data + palette_size;
    runs.size = payload->size - palette_size;
    runs.cap = runs.size;
    return dec_irun(&runs, h, payload->data, out);
  }
  if (h->transform == TRANSFORM_CPAL_DELTA) {
    uint8_t *palette = (uint8_t *)xmalloc(palette_size);
    if (!palette)
      return 0;
    for (size_t i = 0; i < palette_size; ++i)
      palette[i] = i < 4u ? payload->data[i]
                          : (uint8_t)(palette[i - 4u] + payload->data[i]);
    Buf runs = {0};
    runs.data = payload->data + palette_size;
    runs.size = payload->size - palette_size;
    runs.cap = runs.size;
    int ok = dec_irun(&runs, h, palette, out);
    free(palette);
    return ok;
  }
  size_t row_bytes = row_pack(h->width, h->index_bits);
  size_t index_size = 0;
  if (!mulok(row_bytes, (size_t)h->height, &index_size))
    return 0;
  if (payload->size != palette_size + index_size) {
    set_err("corrupt file: cpalette payload size mismatch");
    return 0;
  }
  Buf samples = {0};
  samples.data = payload->data + palette_size;
  samples.size = index_size;
  samples.cap = index_size;
  QlicHeader ph = *h;
  ph.mode = MODE_PALETTE;
  return samp_rgba(&samples, &ph, payload->data, out);
}

enum { ANIM_TASK_ERROR, ANIM_TASK_ALLOC, ANIM_TASK_LIMIT };

enum { ANIM_FRAME_KEY, ANIM_FRAME_DUPLICATE, ANIM_FRAME_RECT, ANIM_FRAME_MOVE };

typedef struct {
  const uint8_t *data;
  size_t size;
  const QlicDecodeLimits *limits;
  Image *image;
  unsigned inner_threads;
  int error;
  int ok;
} AnimDecTask;

static void anim_dec_one(AnimDecTask *task) {
  unsigned previous = g_threads;
  clear_err();
  qlic_core_set_thread_count(task->inner_threads);
  task->ok =
      dec_qlic_limited(task->data, task->size, task->image, NULL, task->limits);
  if (!task->ok) {
    QlicCoreStatus status = qlic_core_status();
    if (status == QLIC_CORE_OUT_OF_MEMORY)
      task->error = ANIM_TASK_ALLOC;
    else if (status == QLIC_CORE_LIMIT_EXCEEDED)
      task->error = ANIM_TASK_LIMIT;
  }
  qlic_core_set_thread_count(previous);
}

static void anim_dec_item(void *context, unsigned index) {
  anim_dec_one(&((AnimDecTask *)context)[index]);
}

static int dec_anim2_payload(const Buf *payload, const QlicHeader *h, Anim *out,
                             const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  if (payload->size < 12 || memcmp(payload->data, "QAN2", 4) != 0) {
    set_err("corrupt file: invalid animation payload");
    return 0;
  }
  uint32_t count = rd32(payload->data + 4);
  uint32_t loop = rd32(payload->data + 8);
  if (!count || count != h->palette_count) {
    set_err("corrupt file: invalid animation frame count");
    return 0;
  }
  if (count > limits->max_frames) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: animation frames");
    return 0;
  }
  uint64_t frame_bytes = (uint64_t)h->width * h->height * 4u;
  uint64_t storage = (uint64_t)count * sizeof(AnimFrame);
  uint64_t slots = (uint64_t)count + 1u;
  /* a rectangle briefly needs its decoded patch beside the finished frames */
  if (storage > limits->max_animation_bytes ||
      frame_bytes > (limits->max_animation_bytes - storage) / slots) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: animation memory");
    return 0;
  }
  size_t frame_size = (size_t)frame_bytes;
  AnimFrame *frames = (AnimFrame *)calloc(count, sizeof(*frames));
  if (!frames) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  size_t pos = 12;
  int ok = 1;
  for (uint32_t i = 0; i < count && ok; ++i) {
    if (payload->size - pos < 8u) {
      set_err("corrupt file: truncated animation table");
      ok = 0;
      break;
    }
    uint32_t delay = rd32(payload->data + pos);
    uint32_t type = rd32(payload->data + pos + 4u);
    pos += 8u;
    frames[i].delay_ms = delay ? delay : 100u;
    if (i == 0 && type != ANIM_FRAME_KEY) {
      set_err("corrupt file: animation must start with a key frame");
      ok = 0;
      break;
    }
    if (type == ANIM_FRAME_DUPLICATE) {
      uint8_t *rgba = (uint8_t *)xmalloc(frame_size);
      if (!rgba) {
        ok = 0;
        break;
      }
      memcpy(rgba, frames[i - 1u].image.rgba, frame_size);
      frames[i].image.width = h->width;
      frames[i].image.height = h->height;
      frames[i].image.rgba = rgba;
      continue;
    }
    if (type == ANIM_FRAME_MOVE) {
      if (payload->size - pos < 28u) {
        set_err("corrupt file: truncated animation move");
        ok = 0;
        break;
      }
      uint32_t source_x = rd32(payload->data + pos);
      uint32_t source_y = rd32(payload->data + pos + 4u);
      uint32_t destination_x = rd32(payload->data + pos + 8u);
      uint32_t destination_y = rd32(payload->data + pos + 12u);
      uint32_t width = rd32(payload->data + pos + 16u);
      uint32_t height = rd32(payload->data + pos + 20u);
      uint32_t clear = rd32(payload->data + pos + 24u);
      pos += 28u;
      if (!width || !height || source_x >= h->width || source_y >= h->height ||
          destination_x >= h->width || destination_y >= h->height ||
          width > h->width - source_x || height > h->height - source_y ||
          width > h->width - destination_x ||
          height > h->height - destination_y) {
        set_err("corrupt file: invalid animation move");
        ok = 0;
        break;
      }
      uint8_t *rgba = (uint8_t *)xmalloc(frame_size);
      if (!rgba) {
        ok = 0;
        break;
      }
      memcpy(rgba, frames[i - 1u].image.rgba, frame_size);
      uint8_t color[4] = {(uint8_t)clear, (uint8_t)(clear >> 8),
                          (uint8_t)(clear >> 16), (uint8_t)(clear >> 24)};
      size_t stride = (size_t)h->width * 4u;
      for (uint32_t yy = 0; yy < height; ++yy) {
        uint8_t *row =
            rgba + (size_t)(source_y + yy) * stride + (size_t)source_x * 4u;
        for (uint32_t xx = 0; xx < width; ++xx)
          memcpy(row + (size_t)xx * 4u, color, 4u);
      }
      /* overlap is safe because the source is always the previous frame */
      for (uint32_t yy = 0; yy < height; ++yy)
        memcpy(rgba + (size_t)(destination_y + yy) * stride +
                   (size_t)destination_x * 4u,
               frames[i - 1u].image.rgba + (size_t)(source_y + yy) * stride +
                   (size_t)source_x * 4u,
               (size_t)width * 4u);
      frames[i].image.width = h->width;
      frames[i].image.height = h->height;
      frames[i].image.rgba = rgba;
      continue;
    }
    uint32_t x = 0, y = 0, width = h->width, height = h->height;
    if (type == ANIM_FRAME_RECT) {
      if (payload->size - pos < 24u) {
        set_err("corrupt file: truncated animation rectangle");
        ok = 0;
        break;
      }
      x = rd32(payload->data + pos);
      y = rd32(payload->data + pos + 4u);
      width = rd32(payload->data + pos + 8u);
      height = rd32(payload->data + pos + 12u);
      pos += 16u;
      if (!width || !height || x >= h->width || y >= h->height ||
          width > h->width - x || height > h->height - y) {
        set_err("corrupt file: invalid animation rectangle");
        ok = 0;
        break;
      }
    } else if (type != ANIM_FRAME_KEY) {
      set_err("corrupt file: invalid animation frame type");
      ok = 0;
      break;
    }
    if (payload->size - pos < 8u) {
      set_err("corrupt file: truncated animation frame");
      ok = 0;
      break;
    }
    uint64_t n64 = rd64(payload->data + pos);
    pos += 8u;
    if (n64 > SIZE_MAX || (size_t)n64 > payload->size - pos) {
      set_err("corrupt file: invalid animation frame size");
      ok = 0;
      break;
    }
    size_t frame_file_size = (size_t)n64;
    QlicHeader frame_head = {0};
    if (!rd_head_limited(payload->data + pos, frame_file_size, &frame_head,
                         limits)) {
      ok = 0;
      break;
    }
    if (frame_head.mode == MODE_ANIM) {
      set_err("corrupt file: nested animation frame");
      ok = 0;
      break;
    }
    if (frame_head.width != width || frame_head.height != height) {
      set_err("corrupt file: animation frame dimensions mismatch");
      ok = 0;
      break;
    }
    Image decoded = {0};
    if (!dec_qlic_limited(payload->data + pos, frame_file_size, &decoded, NULL,
                          limits)) {
      ok = 0;
      break;
    }
    pos += frame_file_size;
    if (type == ANIM_FRAME_KEY) {
      frames[i].image = decoded;
      continue;
    }
    uint8_t *rgba = (uint8_t *)xmalloc(frame_size);
    if (!rgba) {
      image_free(&decoded);
      ok = 0;
      break;
    }
    memcpy(rgba, frames[i - 1u].image.rgba, frame_size);
    size_t dst_stride = (size_t)h->width * 4u;
    size_t src_stride = (size_t)width * 4u;
    for (uint32_t yy = 0; yy < height; ++yy)
      memcpy(rgba + (size_t)(y + yy) * dst_stride + (size_t)x * 4u,
             decoded.rgba + (size_t)yy * src_stride, src_stride);
    image_free(&decoded);
    frames[i].image.width = h->width;
    frames[i].image.height = h->height;
    frames[i].image.rgba = rgba;
  }
  if (ok && pos != payload->size) {
    set_err("corrupt file: trailing animation data");
    ok = 0;
  }
  if (!ok) {
    Anim tmp = {0};
    tmp.frames = frames;
    tmp.count = count;
    anim_free(&tmp);
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  out->count = count;
  out->loop_count = loop;
  out->frames = frames;
  return 1;
}

static int dec_anim_payload(const Buf *payload, const QlicHeader *h, Anim *out,
                            const QlicDecodeLimits *decode_limit) {
  if (payload->size >= 4 && memcmp(payload->data, "QAN2", 4) == 0)
    return dec_anim2_payload(payload, h, out, decode_limit);
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  if (payload->size < 12 || memcmp(payload->data, "QAN1", 4) != 0) {
    set_err("corrupt file: invalid animation payload");
    return 0;
  }
  uint32_t count = rd32(payload->data + 4);
  uint32_t loop = rd32(payload->data + 8);
  if (count == 0 || count != h->palette_count) {
    set_err("corrupt file: invalid animation frame count");
    return 0;
  }
  if (count > limits->max_frames) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: animation frames");
    return 0;
  }
  unsigned budget = g_threads ? g_threads : 1u;
  unsigned workers = budget;
  if (workers > count)
    workers = count;
  uint32_t capacity = workers > count / 2u ? count : workers * 2u;
  uint64_t frame_bytes = (uint64_t)h->width * h->height * 4u;
  uint64_t storage = (uint64_t)count * sizeof(AnimFrame);
  uint64_t task_bytes = (uint64_t)capacity * sizeof(AnimDecTask);
  if (storage > limits->max_animation_bytes ||
      frame_bytes > (limits->max_animation_bytes - storage) / count ||
      task_bytes >
          limits->max_animation_bytes - storage - frame_bytes * count) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: animation memory");
    return 0;
  }
  AnimFrame *frames = (AnimFrame *)calloc(count, sizeof(*frames));
  if (!frames) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  AnimDecTask *tasks = (AnimDecTask *)calloc(capacity, sizeof(*tasks));
  if (!tasks) {
    free(frames);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  size_t pos = 12;
  int ok = 1;
  for (uint32_t base = 0; base < count && ok; base += capacity) {
    uint32_t batch = count - base;
    if (batch > capacity)
      batch = capacity;
    memset(tasks, 0, (size_t)batch * sizeof(*tasks));
    for (uint32_t i = 0; i < batch; ++i) {
      uint32_t index = base + i;
      if (payload->size - pos < 16) {
        set_err("corrupt file: truncated animation table");
        ok = 0;
        break;
      }
      uint32_t delay = rd32(payload->data + pos);
      uint32_t flags = rd32(payload->data + pos + 4);
      uint64_t n64 = rd64(payload->data + pos + 8);
      pos += 16;
      if (flags || n64 > SIZE_MAX || (size_t)n64 > payload->size - pos) {
        set_err("corrupt file: invalid animation frame entry");
        ok = 0;
        break;
      }
      const uint8_t *frame = payload->data + pos;
      if (n64 < QLIC_HEADER_SIZE ||
          memcmp(frame, QLIC_MAGIC, QLIC_MAGIC_SIZE) != 0) {
        set_err("corrupt file: invalid animation frame");
        ok = 0;
        break;
      }
      if (frame[12] == MODE_ANIM) {
        set_err("corrupt file: nested animation frame");
        ok = 0;
        break;
      }
      if (rd32(frame + 4) != h->width || rd32(frame + 8) != h->height) {
        set_err("corrupt file: animation frame dimensions mismatch");
        ok = 0;
        break;
      }
      frames[index].delay_ms = delay ? delay : 100u;
      tasks[i].data = frame;
      tasks[i].size = (size_t)n64;
      tasks[i].limits = limits;
      tasks[i].image = &frames[index].image;
      pos += (size_t)n64;
    }
    if (!ok)
      break;
    unsigned outer = workers;
    if (outer > batch)
      outer = batch;
    unsigned inner = budget / outer;
    unsigned extra = budget % outer;
    for (uint32_t i = 0; i < batch; ++i)
      tasks[i].inner_threads = inner + (i < extra);
    if (outer <= 1u) {
      for (uint32_t i = 0; i < batch; ++i)
        anim_dec_one(&tasks[i]);
    } else {
      qlic_parallel_for(batch, outer, anim_dec_item, tasks);
    }
    for (uint32_t i = 0; i < batch; ++i) {
      if (tasks[i].ok)
        continue;
      if (tasks[i].error == ANIM_TASK_ALLOC)
        set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
      else if (tasks[i].error == ANIM_TASK_LIMIT)
        set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                       "resource limit exceeded: animation frame");
      else
        set_err("corrupt file: animation frame decode failed");
      ok = 0;
      break;
    }
  }
  if (ok && pos != payload->size) {
    set_err("corrupt file: trailing animation data");
    ok = 0;
  }
  free(tasks);
  if (!ok) {
    Anim tmp = {0};
    tmp.frames = frames;
    tmp.count = count;
    anim_free(&tmp);
    return 0;
  }
  out->width = h->width;
  out->height = h->height;
  out->count = count;
  out->loop_count = loop;
  out->frames = frames;
  return 1;
}

typedef struct {
  const uint8_t *p;
  size_t n;
  size_t pos;
} BlockReader;

static int br_need(BlockReader *r, size_t n) {
  if (n > r->n - r->pos) {
    set_err("corrupt file: truncated block stream");
    return 0;
  }
  return 1;
}

static int br_u8(BlockReader *r, uint8_t *v) {
  if (!br_need(r, 1))
    return 0;
  *v = r->p[r->pos++];
  return 1;
}

static int br_color(BlockReader *r, int ch, uint32_t *v) {
  if (!br_need(r, (size_t)ch))
    return 0;
  const uint8_t *p = r->p + r->pos;
  if (ch == 1)
    *v = p[0];
  else if (ch == 3)
    *v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
  else
    *v = rd32(p);
  r->pos += (size_t)ch;
  return 1;
}

static int br_bits(BlockReader *r, uint8_t *idx, size_t n, int bits, int maxv) {
  if (bits <= 0) {
    memset(idx, 0, n);
    return 1;
  }
  size_t bytes = (n * (size_t)bits + 7u) >> 3;
  if (!br_need(r, bytes))
    return 0;
  uint32_t acc = 0;
  int used = 0;
  size_t p = r->pos;
  for (size_t i = 0; i < n; ++i) {
    while (used < bits) {
      acc |= (uint32_t)r->p[p++] << used;
      used += 8;
    }
    uint8_t v = (uint8_t)(acc & ((1u << bits) - 1u));
    if (v >= (uint8_t)maxv) {
      set_err("corrupt file: invalid block index");
      return 0;
    }
    idx[i] = v;
    acc >>= bits;
    used -= bits;
  }
  r->pos += bytes;
  return 1;
}

static void dec_fill_block(Image *out, uint32_t x, uint32_t y, uint32_t bw,
                           uint32_t bh, uint32_t color, int ch) {
  for (uint32_t yy = 0; yy < bh; ++yy)
    for (uint32_t xx = 0; xx < bw; ++xx)
      blk_set(out, x + xx, y + yy, color, ch);
}

static void dec_copy_block(Image *out, uint32_t x, uint32_t y, uint32_t sx,
                           uint32_t sy, uint32_t bw, uint32_t bh, int ch) {
  for (uint32_t yy = 0; yy < bh; ++yy)
    for (uint32_t xx = 0; xx < bw; ++xx)
      blk_set(out, x + xx, y + yy, blk_color(out, sx + xx, sy + yy, ch), ch);
}

static int dec_raw_block(BlockReader *r, Image *out, uint32_t x, uint32_t y,
                         uint32_t bw, uint32_t bh, int ch) {
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      uint32_t c = 0;
      if (!br_color(r, ch, &c))
        return 0;
      blk_set(out, x + xx, y + yy, c, ch);
    }
  }
  return 1;
}

static int dec_map_block(BlockReader *r, Image *out, uint32_t x, uint32_t y,
                         uint32_t bw, uint32_t bh, uint32_t *colors, int count,
                         int ch) {
  uint8_t idx[BLK_SIZE * BLK_SIZE];
  uint32_t area = bw * bh;
  if (!br_bits(r, idx, area, blk_bits(count), count))
    return 0;
  for (uint32_t yy = 0; yy < bh; ++yy) {
    for (uint32_t xx = 0; xx < bw; ++xx) {
      uint8_t k = idx[(size_t)yy * bw + xx];
      blk_set(out, x + xx, y + yy, colors[k], ch);
    }
  }
  return 1;
}

static int dec_pat_block(BlockReader *r, Image *out, uint32_t x, uint32_t y,
                         uint32_t bw, uint32_t bh, int ch) {
  uint8_t n8 = 0;
  if (!br_u8(r, &n8) || n8 < 2 || n8 > 16) {
    set_err("corrupt file: invalid block pattern table");
    return 0;
  }
  BlockPattern pat[16];
  for (int i = 0; i < (int)n8; ++i)
    for (int j = 0; j < 4; ++j)
      if (!br_color(r, ch, &pat[i].v[j]))
        return 0;
  uint32_t pw = (bw + 1u) >> 1;
  uint32_t ph = (bh + 1u) >> 1;
  uint8_t idx[64];
  if (!br_bits(r, idx, (size_t)pw * ph, blk_bits(n8), n8))
    return 0;
  for (uint32_t py = 0; py < ph; ++py) {
    for (uint32_t px = 0; px < pw; ++px) {
      BlockPattern *p = &pat[idx[(size_t)py * pw + px]];
      uint32_t x0 = x + px * 2u;
      uint32_t y0 = y + py * 2u;
      blk_set(out, x0, y0, p->v[0], ch);
      if (px * 2u + 1u < bw)
        blk_set(out, x0 + 1u, y0, p->v[1], ch);
      if (py * 2u + 1u < bh) {
        blk_set(out, x0, y0 + 1u, p->v[2], ch);
        if (px * 2u + 1u < bw)
          blk_set(out, x0 + 1u, y0 + 1u, p->v[3], ch);
      }
    }
  }
  return 1;
}

static int compact_to_image(uint8_t *pix, uint32_t w, uint32_t h, int ch,
                            Image *out) {
  size_t pixels = 0, rgba_bytes = 0;
  if (!mulok((size_t)w, (size_t)h, &pixels) || !mulok(pixels, 4u, &rgba_bytes))
    return 0;
  if (ch == 4) {
    out->rgba = pix;
    out->width = w;
    out->height = h;
    return 1;
  }
  uint8_t *rgba = (uint8_t *)xmalloc(rgba_bytes);
  if (!rgba)
    return 0;
  for (size_t i = 0; i < pixels; ++i) {
    if (ch == 1) {
      uint8_t v = pix[i];
      rgba[i * 4u + 0u] = v;
      rgba[i * 4u + 1u] = v;
      rgba[i * 4u + 2u] = v;
      rgba[i * 4u + 3u] = 255;
    } else {
      rgba[i * 4u + 0u] = pix[i * 3u + 0u];
      rgba[i * 4u + 1u] = pix[i * 3u + 1u];
      rgba[i * 4u + 2u] = pix[i * 3u + 2u];
      rgba[i * 4u + 3u] = 255;
    }
  }
  free(pix);
  out->rgba = rgba;
  out->width = w;
  out->height = h;
  return 1;
}

static int dec_cf_regions(const Buf *payload, const QlicHeader *h, Image *out) {
  if (payload->size < 20 || memcmp(payload->data, "QCF1", 4) != 0 ||
      payload->data[4] != CF_SIZE ||
      (payload->data[5] != 1 && payload->data[5] != 3 &&
       payload->data[5] != 4) ||
      payload->data[6] != 0 || payload->data[7] != 0) {
    set_err("corrupt file: invalid coordinate stream");
    return 0;
  }
  int ch = payload->data[5];
  size_t table_size = (size_t)rd32(payload->data + 8);
  uint64_t z64 = rd64(payload->data + 12);
  if (z64 > SIZE_MAX) {
    set_err("corrupt file: coordinate residual is too large");
    return 0;
  }
  size_t zsize = (size_t)z64;
  size_t end_table = 0, end = 0;
  if (!addok(20u, table_size, &end_table) || !addok(end_table, zsize, &end) ||
      end != payload->size) {
    set_err("corrupt file: coordinate residual size mismatch");
    return 0;
  }
  uint64_t rx = (h->width + CF_SIZE - 1u) / CF_SIZE;
  uint64_t ry = (h->height + CF_SIZE - 1u) / CF_SIZE;
  if (rx && ry && table_size != rx * ry * 3u) {
    set_err("corrupt file: coordinate table size mismatch");
    return 0;
  }
  uint8_t *res = NULL;
  uint32_t w = 0, hh = 0;
  int zch = 0;
  int err =
      stream_decode_trusted_expected(payload->data + end_table, zsize, h->width,
                                     h->height, ch, &res, &w, &hh, &zch);
  if (err != STREAM_OK) {
    set_err_status(stream_failure_status(err, 1),
                   "coordinate residual decode failed: %s",
                   stream_strerror(err));
    return 0;
  }
  if (w != h->width || hh != h->height || zch != ch) {
    stream_free(res);
    set_err("corrupt file: coordinate residual dimensions mismatch");
    return 0;
  }
  size_t pixels = 0, compact = 0;
  if (!mulok((size_t)w, (size_t)hh, &pixels) ||
      !mulok(pixels, (size_t)ch, &compact)) {
    stream_free(res);
    return 0;
  }
  uint8_t *pix = (uint8_t *)xmalloc(compact);
  if (!pix) {
    stream_free(res);
    return 0;
  }
  size_t tpos = 0;
  for (uint32_t y = 0; y < h->height; y += CF_SIZE) {
    for (uint32_t x = 0; x < h->width; x += CF_SIZE) {
      uint32_t bw = block_extent(h->width, x, CF_SIZE);
      uint32_t bh = block_extent(h->height, y, CF_SIZE);
      if (tpos > table_size || table_size - tpos < 3u) {
        free(pix);
        stream_free(res);
        set_err("corrupt file: truncated coordinate table");
        return 0;
      }
      uint8_t par[BLK2_MAX_PAR];
      memset(par, 0, sizeof(par));
      cf_base_buf(pix, w, x, y, bw, bh, ch, par);
      memcpy(par, payload->data + 20u + tpos, 3u);
      tpos += 3u;
      for (uint32_t yy = 0; yy < bh; ++yy) {
        for (uint32_t xx = 0; xx < bw; ++xx) {
          for (int c = 0; c < ch; ++c) {
            size_t pos = blk2_i(w, ch, x + xx, y + yy, c);
            uint8_t p =
                pdm_pred_buf(pix, w, x, y, bw, bh, CF_SIZE, xx, yy, ch, c, par);
            pix[pos] = (uint8_t)((int)p + unfold_delta(res[pos]));
          }
        }
      }
    }
  }
  stream_free(res);
  if (tpos != table_size) {
    free(pix);
    set_err("corrupt file: trailing coordinate table data");
    return 0;
  }
  if (!compact_to_image(pix, w, hh, ch, out)) {
    free(pix);
    return 0;
  }
  return 1;
}

static int dec_blocks2(const Buf *payload, const QlicHeader *h, Image *out) {
  if (payload->size < 20 || memcmp(payload->data, "QBL2", 4) != 0 ||
      payload->data[4] != BLK_SIZE ||
      (payload->data[5] != 1 && payload->data[5] != 3 &&
       payload->data[5] != 4) ||
      payload->data[6] != 0 || payload->data[7] != 0) {
    set_err("corrupt file: invalid block residual stream");
    return 0;
  }
  int ch = payload->data[5];
  size_t table_size = (size_t)rd32(payload->data + 8);
  uint64_t z64 = rd64(payload->data + 12);
  if (z64 > SIZE_MAX) {
    set_err("corrupt file: block residual is too large");
    return 0;
  }
  size_t zsize = (size_t)z64;
  size_t pos = 0, end_table = 0, end = 0;
  if (!addok(20u, table_size, &end_table) || !addok(end_table, zsize, &end) ||
      end != payload->size) {
    set_err("corrupt file: block residual size mismatch");
    return 0;
  }
  uint8_t *res = NULL;
  uint32_t w = 0, hh = 0;
  int zch = 0;
  int err =
      stream_decode_trusted_expected(payload->data + end_table, zsize, h->width,
                                     h->height, ch, &res, &w, &hh, &zch);
  if (err != STREAM_OK) {
    set_err_status(stream_failure_status(err, 1),
                   "block residual decode failed: %s", stream_strerror(err));
    return 0;
  }
  if (w != h->width || hh != h->height || zch != ch) {
    stream_free(res);
    set_err("corrupt file: block residual dimensions mismatch");
    return 0;
  }
  size_t pixels = 0, compact = 0, rgba_bytes = 0;
  if (!mulok((size_t)w, (size_t)hh, &pixels) ||
      !mulok(pixels, (size_t)ch, &compact) || !mulok(pixels, 4u, &rgba_bytes)) {
    stream_free(res);
    return 0;
  }
  uint8_t *pix = (uint8_t *)xmalloc(compact);
  if (!pix) {
    stream_free(res);
    return 0;
  }
  BlockReader r = {payload->data + 20u, table_size, 0};
  for (uint32_t y = 0; y < h->height; y += BLK_SIZE) {
    for (uint32_t x = 0; x < h->width; x += BLK_SIZE) {
      uint32_t bw = block_extent(h->width, x, BLK_SIZE);
      uint32_t bh = block_extent(h->height, y, BLK_SIZE);
      uint8_t op = 0;
      if (!br_u8(&r, &op)) {
        free(pix);
        stream_free(res);
        return 0;
      }
      if (op > BLK2_PDM || (op == BLK2_LEFT && x < BLK_SIZE) ||
          (op == BLK2_UP && y < BLK_SIZE)) {
        free(pix);
        stream_free(res);
        set_err("corrupt file: invalid block residual opcode");
        return 0;
      }
      size_t parn = blk2_parn(op, ch);
      if (!br_need(&r, parn)) {
        free(pix);
        stream_free(res);
        return 0;
      }
      const uint8_t *par = r.p + r.pos;
      r.pos += parn;
      for (uint32_t yy = 0; yy < bh; ++yy) {
        for (uint32_t xx = 0; xx < bw; ++xx) {
          for (int c = 0; c < ch; ++c) {
            pos = blk2_i(w, ch, x + xx, y + yy, c);
            uint8_t p =
                blk2_pred_buf(pix, w, x, y, bw, bh, xx, yy, ch, c, op, par);
            pix[pos] = (uint8_t)((int)p + unfold_delta(res[pos]));
          }
        }
      }
    }
  }
  stream_free(res);
  if (r.pos != r.n) {
    free(pix);
    set_err("corrupt file: trailing block residual table data");
    return 0;
  }
  if (ch == 4) {
    out->rgba = pix;
    out->width = w;
    out->height = hh;
    return 1;
  }
  uint8_t *rgba = (uint8_t *)xmalloc(rgba_bytes);
  if (!rgba) {
    free(pix);
    return 0;
  }
  for (size_t i = 0; i < pixels; ++i) {
    if (ch == 1) {
      uint8_t v = pix[i];
      rgba[i * 4u + 0u] = v;
      rgba[i * 4u + 1u] = v;
      rgba[i * 4u + 2u] = v;
      rgba[i * 4u + 3u] = 255;
    } else {
      rgba[i * 4u + 0u] = pix[i * 3u + 0u];
      rgba[i * 4u + 1u] = pix[i * 3u + 1u];
      rgba[i * 4u + 2u] = pix[i * 3u + 2u];
      rgba[i * 4u + 3u] = 255;
    }
  }
  free(pix);
  out->rgba = rgba;
  out->width = w;
  out->height = hh;
  return 1;
}

static int dec_pdm_regions(const Buf *payload, const QlicHeader *h,
                           Image *out) {
  if (payload->size < 20 || memcmp(payload->data, "QPD1", 4) != 0 ||
      payload->data[4] != PDM_SIZE ||
      (payload->data[5] != 1 && payload->data[5] != 3 &&
       payload->data[5] != 4) ||
      payload->data[6] != 0 || payload->data[7] != 0) {
    set_err("corrupt file: invalid pdm stream");
    return 0;
  }
  int ch = payload->data[5];
  size_t table_size = (size_t)rd32(payload->data + 8);
  uint64_t z64 = rd64(payload->data + 12);
  if (z64 > SIZE_MAX) {
    set_err("corrupt file: pdm residual is too large");
    return 0;
  }
  size_t zsize = (size_t)z64;
  size_t pos = 0, end_table = 0, end = 0;
  if (!addok(20u, table_size, &end_table) || !addok(end_table, zsize, &end) ||
      end != payload->size) {
    set_err("corrupt file: pdm residual size mismatch");
    return 0;
  }
  uint8_t *res = NULL;
  uint32_t w = 0, hh = 0;
  int zch = 0;
  int err =
      stream_decode_trusted_expected(payload->data + end_table, zsize, h->width,
                                     h->height, ch, &res, &w, &hh, &zch);
  if (err != STREAM_OK) {
    set_err_status(stream_failure_status(err, 1),
                   "pdm residual decode failed: %s", stream_strerror(err));
    return 0;
  }
  if (w != h->width || hh != h->height || zch != ch) {
    stream_free(res);
    set_err("corrupt file: pdm residual dimensions mismatch");
    return 0;
  }
  size_t pixels = 0, compact = 0, rgba_bytes = 0;
  if (!mulok((size_t)w, (size_t)hh, &pixels) ||
      !mulok(pixels, (size_t)ch, &compact) || !mulok(pixels, 4u, &rgba_bytes)) {
    stream_free(res);
    return 0;
  }
  uint8_t *pix = (uint8_t *)xmalloc(compact);
  if (!pix) {
    stream_free(res);
    return 0;
  }
  BlockReader r = {payload->data + 20u, table_size, 0};
  size_t parn = blk2_parn(BLK2_PDM, ch);
  for (uint32_t y = 0; y < h->height; y += PDM_SIZE) {
    for (uint32_t x = 0; x < h->width; x += PDM_SIZE) {
      uint32_t bw = block_extent(h->width, x, PDM_SIZE);
      uint32_t bh = block_extent(h->height, y, PDM_SIZE);
      if (!br_need(&r, parn)) {
        free(pix);
        stream_free(res);
        return 0;
      }
      const uint8_t *par = r.p + r.pos;
      r.pos += parn;
      for (uint32_t yy = 0; yy < bh; ++yy) {
        for (uint32_t xx = 0; xx < bw; ++xx) {
          for (int c = 0; c < ch; ++c) {
            pos = blk2_i(w, ch, x + xx, y + yy, c);
            uint8_t p = pdm_pred_buf(pix, w, x, y, bw, bh, PDM_SIZE, xx, yy, ch,
                                     c, par);
            pix[pos] = (uint8_t)((int)p + unfold_delta(res[pos]));
          }
        }
      }
    }
  }
  stream_free(res);
  if (r.pos != r.n) {
    free(pix);
    set_err("corrupt file: trailing pdm table data");
    return 0;
  }
  if (ch == 4) {
    out->rgba = pix;
    out->width = w;
    out->height = hh;
    return 1;
  }
  uint8_t *rgba = (uint8_t *)xmalloc(rgba_bytes);
  if (!rgba) {
    free(pix);
    return 0;
  }
  for (size_t i = 0; i < pixels; ++i) {
    if (ch == 1) {
      uint8_t v = pix[i];
      rgba[i * 4u + 0u] = v;
      rgba[i * 4u + 1u] = v;
      rgba[i * 4u + 2u] = v;
      rgba[i * 4u + 3u] = 255;
    } else {
      rgba[i * 4u + 0u] = pix[i * 3u + 0u];
      rgba[i * 4u + 1u] = pix[i * 3u + 1u];
      rgba[i * 4u + 2u] = pix[i * 3u + 2u];
      rgba[i * 4u + 3u] = 255;
    }
  }
  free(pix);
  out->rgba = rgba;
  out->width = w;
  out->height = hh;
  return 1;
}

static int dec_blocks(const Buf *payload, const QlicHeader *h, Image *out) {
  if (payload->size >= 4 && memcmp(payload->data, "QCF1", 4) == 0)
    return dec_cf_regions(payload, h, out);
  if (payload->size >= 4 && memcmp(payload->data, "QPD1", 4) == 0)
    return dec_pdm_regions(payload, h, out);
  if (payload->size >= 4 && memcmp(payload->data, "QBL2", 4) == 0)
    return dec_blocks2(payload, h, out);
  int extended = payload->size >= 4 && memcmp(payload->data, "QBR1", 4) == 0;
  if (payload->size < 8 ||
      (!extended && memcmp(payload->data, "QBL1", 4) != 0) ||
      payload->data[4] != BLK_SIZE ||
      (payload->data[5] != 1 && payload->data[5] != 3 &&
       payload->data[5] != 4) ||
      payload->data[6] != 0 || payload->data[7] != 0) {
    set_err("corrupt file: invalid block stream");
    return 0;
  }
  int ch = payload->data[5];
  size_t pixels = 0, bytes = 0;
  if (!mulok((size_t)h->width, (size_t)h->height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  uint8_t *rgba = (uint8_t *)xmalloc(bytes);
  if (!rgba)
    return 0;
  Image im = {h->width, h->height, rgba};
  BlockReader r = {payload->data, payload->size, 8};
  uint32_t blocks_x = (h->width + BLK_SIZE - 1u) / BLK_SIZE;
  size_t block_index = 0;
  for (uint32_t y = 0; y < h->height; y += BLK_SIZE) {
    for (uint32_t x = 0; x < h->width; x += BLK_SIZE) {
      uint32_t bw = block_extent(h->width, x, BLK_SIZE);
      uint32_t bh = block_extent(h->height, y, BLK_SIZE);
      uint8_t op = 0;
      uint32_t colors[4];
      if (!br_u8(&r, &op)) {
        image_free(&im);
        return 0;
      }
      if (op == BLK_RAW) {
        if (!dec_raw_block(&r, &im, x, y, bw, bh, ch)) {
          image_free(&im);
          return 0;
        }
      } else if (op == BLK_FLAT) {
        uint32_t c = 0;
        if (!br_color(&r, ch, &c)) {
          image_free(&im);
          return 0;
        }
        dec_fill_block(&im, x, y, bw, bh, c, ch);
      } else if (op == BLK_LEFT) {
        if (x < BLK_SIZE) {
          image_free(&im);
          set_err("corrupt file: invalid left block copy");
          return 0;
        }
        dec_copy_block(&im, x, y, x - BLK_SIZE, y, bw, bh, ch);
      } else if (op == BLK_UP) {
        if (y < BLK_SIZE) {
          image_free(&im);
          set_err("corrupt file: invalid upper block copy");
          return 0;
        }
        dec_copy_block(&im, x, y, x, y - BLK_SIZE, bw, bh, ch);
      } else if (op == BLK_TWO) {
        if (!br_color(&r, ch, &colors[0]) || !br_color(&r, ch, &colors[1]) ||
            !dec_map_block(&r, &im, x, y, bw, bh, colors, 2, ch)) {
          image_free(&im);
          return 0;
        }
      } else if (op == BLK_FOUR) {
        uint8_t count = 0;
        if (!br_u8(&r, &count) || count < 3 || count > 4) {
          image_free(&im);
          set_err("corrupt file: invalid block palette");
          return 0;
        }
        for (int i = 0; i < (int)count; ++i) {
          if (!br_color(&r, ch, &colors[i])) {
            image_free(&im);
            return 0;
          }
        }
        if (!dec_map_block(&r, &im, x, y, bw, bh, colors, count, ch)) {
          image_free(&im);
          return 0;
        }
      } else if (op == BLK_PAT2) {
        if (!dec_pat_block(&r, &im, x, y, bw, bh, ch)) {
          image_free(&im);
          return 0;
        }
      } else if (op == BLK_REF && extended) {
        size_t distance = 0;
        if (!read_varint(r.p, r.n, &r.pos, &distance) || !distance ||
            distance > block_index) {
          image_free(&im);
          set_err("corrupt file: invalid block reference");
          return 0;
        }
        size_t source = block_index - distance;
        uint32_t sx = (uint32_t)(source % blocks_x) * BLK_SIZE;
        uint32_t sy = (uint32_t)(source / blocks_x) * BLK_SIZE;
        if (sx > h->width - bw || sy > h->height - bh) {
          image_free(&im);
          set_err("corrupt file: invalid block reference");
          return 0;
        }
        dec_copy_block(&im, x, y, sx, sy, bw, bh, ch);
      } else {
        image_free(&im);
        set_err("corrupt file: invalid block opcode");
        return 0;
      }
      ++block_index;
    }
  }
  if (r.pos != r.n) {
    image_free(&im);
    set_err("corrupt file: trailing block data");
    return 0;
  }
  *out = im;
  return 1;
}

static int dec_direct_payload(const Buf *payload, const QlicHeader *h,
                              const uint8_t *palette, Image *out,
                              const QlicDecodeLimits *limits) {
  switch (h->mode) {
  case MODE_ANIM: {
    Anim a = {0};
    if (!dec_anim_payload(payload, h, &a, limits))
      return 0;
    if (!a.count) {
      anim_free(&a);
      set_err("corrupt file: empty animation");
      return 0;
    }
    *out = a.frames[0].image;
    memset(&a.frames[0].image, 0, sizeof(a.frames[0].image));
    anim_free(&a);
    return 1;
  }
  case MODE_NATIVE:
    return dec_stream(payload, h, out);
  case MODE_BLOCKS:
    return dec_blocks(payload, h, out);
  case MODE_TILES:
    return dec_tile(payload, h, out, limits);
  case MODE_TILE_MODEL:
    return dec_rtt(payload, h, out);
  case MODE_GMODEL:
    return dec_gmodel(payload, h, out);
  case MODE_FILTERED:
    return dec_filtered(payload, h, out);
  case MODE_PSTREAM:
    return dec_pstream(payload, h, palette, out);
  case MODE_PPAL:
    return dec_ppal(payload, h, palette, out);
  case MODE_CPAL:
    return dec_cpal(payload, h, out);
  case MODE_SEPARABLE:
    return dec_sep(payload, h, out);
  case MODE_PALETTE:
    if (h->transform == TRANSFORM_INDEX_RLE)
      return dec_irun(payload, h, palette, out);
    break;
  }
  return -1;
}

int dec_qlic_limited(const uint8_t *data, size_t size, Image *out,
                     QlicHeader *header_out,
                     const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  QlicHeader h = {0};
  if (!out) {
    set_err("invalid decoder output");
    return 0;
  }
  if (!rd_head_limited(data, size, &h, limits))
    return 0;
  if (h.mode == MODE_NATIVE_WIDE || h.mode == MODE_HDR_WIDE) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   h.mode == MODE_HDR_WIDE
                       ? "HDR QLIC image requires qlic_decode_hdr"
                       : "wide QLIC image requires qlic_decode_wide");
    return 0;
  }
  size_t palette_size = file_palette_size(&h);
  size_t comp_size = (size_t)h.compressed_size;
  size_t need1, need2;
  if (!addok(QLIC_HEADER_SIZE, palette_size, &need1) ||
      !addok(need1, comp_size, &need2))
    return 0;
  size_t body_size = size - QLIC_FOOTER_SIZE;
  if (need2 != body_size) {
    set_err("corrupt file: file size does not match header");
    return 0;
  }
  const uint8_t *palette = data + QLIC_HEADER_SIZE;
  const uint8_t *comp = palette + palette_size;

  Buf payload = {0};
  if (!unzip(comp, comp_size, h.codec, h.payload_size, &payload, limits)) {
    unzip_free(&payload, h.codec);
    return 0;
  }

  int direct = dec_direct_payload(&payload, &h, palette, out, limits);
  if (direct >= 0) {
    unzip_free(&payload, h.codec);
    if (direct && (out->width != h.width || out->height != h.height)) {
      image_free(out);
      set_err("corrupt file: decoded dimensions mismatch");
      return 0;
    }
    if (direct && header_out)
      *header_out = h;
    return direct;
  }

  if (is_planar_med(h.transform)) {
    int ok = planar_med_rgba(&payload, &h, out);
    unzip_free(&payload, h.codec);
    if (ok && header_out)
      *header_out = h;
    return ok;
  }

  if (is_rle(h.transform)) {
    size_t expected = 0;
    Buf samples = {0};
    if (!samp_size(&h, &expected) ||
        !rle_decode(payload.data, payload.size, expected, &samples)) {
      unzip_free(&payload, h.codec);
      buf_free(&samples);
      return 0;
    }
    unzip_free(&payload, h.codec);
    int ok = samp_rgba(&samples, &h, palette, out);
    buf_free(&samples);
    if (ok && header_out)
      *header_out = h;
    return ok;
  }

  if (is_raw(h.transform)) {
    int ok = samp_rgba(&payload, &h, palette, out);
    unzip_free(&payload, h.codec);
    if (ok && header_out)
      *header_out = h;
    return ok;
  }

  int bpp = mbpp(h.mode);
  size_t row_bytes = 0;
  if (h.mode == MODE_PALETTE) {
    if (!valid_index_bits(h.index_bits)) {
      unzip_free(&payload, h.codec);
      set_err("corrupt file: invalid palette index bit width");
      return 0;
    }
    row_bytes = row_pack(h.width, h.index_bits);
    bpp = h.index_bits == 16 ? 2 : 1;
  } else {
    if (bpp <= 0 || !mulok((size_t)h.width, (size_t)bpp, &row_bytes)) {
      unzip_free(&payload, h.codec);
      set_err("corrupt file: invalid sample mode");
      return 0;
    }
  }
  Buf samples = {0};
  if (!unf_rows(payload.data, payload.size, row_bytes, h.height, bpp,
                &samples)) {
    unzip_free(&payload, h.codec);
    buf_free(&samples);
    return 0;
  }
  unzip_free(&payload, h.codec);
  int ok = samp_rgba(&samples, &h, palette, out);
  buf_free(&samples);
  if (ok && header_out)
    *header_out = h;
  return ok;
}

int dec_qlic_rgba_into_limited(const uint8_t *data, size_t size, uint8_t *rgba,
                               size_t rgba_size, size_t stride, uint32_t *width,
                               uint32_t *height,
                               const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  QlicHeader h = {0};
  if (width)
    *width = 0;
  if (height)
    *height = 0;
  if (!data || !rgba || !width || !height) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid decoder output");
    return 0;
  }
  if (!rd_head_limited(data, size, &h, limits))
    return 0;
  if (h.mode == MODE_NATIVE_WIDE || h.mode == MODE_HDR_WIDE) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   h.mode == MODE_HDR_WIDE
                       ? "HDR QLIC image requires qlic_decode_hdr"
                       : "wide QLIC image requires qlic_decode_wide");
    return 0;
  }
  size_t row = 0;
  size_t required = 0;
  if (!mulok((size_t)h.width, 4u, &row) || stride < row ||
      (h.height > 1u && stride > (SIZE_MAX - row) / (h.height - 1u))) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid RGBA destination layout");
    return 0;
  }
  required = (size_t)(h.height - 1u) * stride + row;
  if (rgba_size < required) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "RGBA destination is too small");
    return 0;
  }

  if (h.mode == MODE_NATIVE && stride == row) {
    size_t body_size = size - QLIC_FOOTER_SIZE;
    size_t compressed_size = (size_t)h.compressed_size;
    if (QLIC_HEADER_SIZE > body_size ||
        compressed_size != body_size - QLIC_HEADER_SIZE) {
      set_err("corrupt file: file size does not match header");
      return 0;
    }
    Buf payload = {0};
    if (!unzip(data + QLIC_HEADER_SIZE, compressed_size, h.codec,
               h.payload_size, &payload, limits)) {
      unzip_free(&payload, h.codec);
      return 0;
    }
    uint32_t decoded_width = 0;
    uint32_t decoded_height = 0;
    int decoded_channels = 0;
    int err = stream_decode_trusted_expected_rgba_into(
        payload.data, payload.size, h.width, h.height, 0, rgba, rgba_size,
        &decoded_width, &decoded_height, &decoded_channels);
    unzip_free(&payload, h.codec);
    if (err != STREAM_OK) {
      set_err_status(stream_failure_status(err, 1),
                     "native stream decode failed: %s", stream_strerror(err));
      return 0;
    }
    if (decoded_width != h.width || decoded_height != h.height ||
        decoded_channels != 4) {
      set_err(
          "corrupt file: native stream dimensions or channel count mismatch");
      return 0;
    }
    *width = h.width;
    *height = h.height;
    return 1;
  }

  Image image = {0};
  if (!dec_qlic_limited(data, size, &image, NULL, limits)) {
    image_free(&image);
    return 0;
  }
  for (uint32_t y = 0; y < image.height; ++y)
    memcpy(rgba + (size_t)y * stride, image.rgba + (size_t)y * row, row);
  *width = image.width;
  *height = image.height;
  image_free(&image);
  return 1;
}

int dec_qlic(const uint8_t *data, size_t size, Image *out,
             QlicHeader *header_out) {
  return dec_qlic_limited(data, size, out, header_out, NULL);
}

enum {
  QSW_HEADER_SIZE = 16,
  QSW_METHOD_BYTE_SLICES = 0,
  QSW2_HEADER_SIZE = 32,
  QSW2_CHUNK_HEADER_SIZE = 16,
  QSW2_VERSION = 1,
  QSW2_SAMPLE_UINT = 1,
  QSW2_CHUNK_CRITICAL = 1,
  QSW2_ALPHA_NONE = 0,
  QSW2_ALPHA_STRAIGHT = 1,
  QSW2_ALPHA_PREMULTIPLIED = 2,
  QSW2_COLOR_UNSPECIFIED = 0,
  QSW2_COLOR_ICC = 1,
  QSW2_COLOR_CICP = 2,
  QSW2_COLOR_ICC_PREFERRED = 3,
  QSW2_COLOR_CICP_PREFERRED = 4
};

static int wide_layout(uint32_t width, uint32_t height, size_t stride,
                       uint32_t channels, uint32_t bits_per_sample,
                       size_t *storage, size_t *row_bytes, size_t *required) {
  if (!width || !height ||
      (channels != 1u && channels != 3u && channels != 4u) ||
      bits_per_sample < 8u || bits_per_sample > 24u)
    return 0;
  *storage = bits_per_sample <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t samples_per_row = 0;
  if (!mulok((size_t)width, (size_t)channels, &samples_per_row) ||
      !mulok(samples_per_row, *storage, row_bytes) || stride < *row_bytes ||
      !mulok((size_t)(height - 1u), stride, required) ||
      !addok(*required, *row_bytes, required))
    return 0;
  return 1;
}

static int wide_fill_slice(const void *pixels, uint32_t width, uint32_t height,
                           size_t stride, uint32_t channels,
                           uint32_t bits_per_sample, size_t storage,
                           uint32_t slice_index, uint8_t *slice) {
  const uint8_t *base = (const uint8_t *)pixels;
  uint32_t maximum = (UINT32_C(1) << bits_per_sample) - 1u;
  size_t pixel_bytes = (size_t)channels * storage;
  size_t output = 0;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t *row = base + (size_t)y * stride;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t *pixel = row + (size_t)x * pixel_bytes;
      for (uint32_t channel = 0; channel < channels; ++channel) {
        uint32_t value;
        if (storage == sizeof(uint16_t)) {
          uint16_t sample;
          memcpy(&sample, pixel + (size_t)channel * storage, sizeof(sample));
          value = sample;
        } else {
          memcpy(&value, pixel + (size_t)channel * storage, sizeof(value));
        }
        if (value > maximum) {
          set_err_status(QLIC_CORE_BAD_ARGUMENT,
                         "wide sample exceeds declared precision");
          return 0;
        }
        slice[output++] = (uint8_t)(value >> (slice_index * 8u));
      }
    }
  }
  return 1;
}

static int append_wide_payload(const void *pixels, size_t pixels_size,
                               uint32_t width, uint32_t height, size_t stride,
                               uint32_t channels, uint32_t bits_per_sample,
                               Buf *payload) {
  if (!pixels || !payload) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid wide encoder input");
    return 0;
  }
  size_t payload_start = payload->size;
  size_t storage = 0, row_bytes = 0, required = 0;
  if (!wide_layout(width, height, stride, channels, bits_per_sample, &storage,
                   &row_bytes, &required) ||
      pixels_size < required) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid wide image layout");
    return 0;
  }
  (void)row_bytes;
  size_t pixel_count = 0, slice_bytes = 0;
  if (!mulok((size_t)width, (size_t)height, &pixel_count) ||
      !mulok(pixel_count, (size_t)channels, &slice_bytes)) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "wide image dimensions overflow");
    return 0;
  }
  uint8_t *slice = (uint8_t *)xmalloc(slice_bytes);
  if (!slice)
    return 0;
  uint32_t slice_count = (bits_per_sample + 7u) / 8u;
  uint32_t checksum = stream_crc32_wide(pixels, width, height, stride, channels,
                                        bits_per_sample);
  int ok = buf_append(payload, "QSW1", 4u) &&
           buf_u8(payload, QSW_METHOD_BYTE_SLICES) &&
           buf_u8(payload, (uint8_t)bits_per_sample) &&
           buf_u8(payload, (uint8_t)channels) &&
           buf_u8(payload, (uint8_t)slice_count) &&
           buf_u32le(payload, checksum) && buf_u32le(payload, 0u);
  for (uint32_t index = 0; index < slice_count && ok; ++index)
    ok = buf_u64le(payload, 0u);
  for (uint32_t index = 0; index < slice_count && ok; ++index) {
    if (!wide_fill_slice(pixels, width, height, stride, channels,
                         bits_per_sample, storage, index, slice)) {
      ok = 0;
      break;
    }
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    int error = stream_encode_threads(slice, width, height, (int)channels, 0,
                                      1u, &encoded, &encoded_size);
    if (error != STREAM_OK) {
      set_err_status(stream_failure_status(error, 0),
                     "wide byte-slice encode failed: %s",
                     stream_strerror(error));
      stream_free(encoded);
      ok = 0;
      break;
    }
    if (!buf_append(payload, encoded, encoded_size)) {
      stream_free(encoded);
      ok = 0;
      break;
    }
    stream_free(encoded);
    wr64(payload->data + payload_start + QSW_HEADER_SIZE + (size_t)index * 8u,
         (uint64_t)encoded_size);
  }
  free(slice);
  if (!ok) {
    payload->size = payload_start;
    return 0;
  }
  return 1;
}

static int enc_wide_payload(const void *pixels, size_t pixels_size,
                            uint32_t width, uint32_t height, size_t stride,
                            uint32_t channels, uint32_t bits_per_sample,
                            Buf *payload) {
  if (payload)
    memset(payload, 0, sizeof(*payload));
  if (!append_wide_payload(pixels, pixels_size, width, height, stride, channels,
                           bits_per_sample, payload)) {
    if (payload)
      buf_free(payload);
    return 0;
  }
  return 1;
}

int enc_wide_mem(const void *pixels, size_t pixels_size, uint32_t width,
                 uint32_t height, size_t stride, uint32_t channels,
                 uint32_t bits_per_sample, Buf *file) {
  if (file)
    memset(file, 0, sizeof(*file));
  if (!file) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid wide encoder output");
    return 0;
  }
  if (bits_per_sample < 9u || bits_per_sample > 24u) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT,
                   "wide images require 9 to 24 bits per sample");
    return 0;
  }
  Buf payload = {0};
  if (!enc_wide_payload(pixels, pixels_size, width, height, stride, channels,
                        bits_per_sample, &payload))
    return 0;
  Candidate best = {0};
  if (!try_store_owned(&best, &payload, MODE_NATIVE_WIDE, TRANSFORM_IDENTITY,
                       (int)bits_per_sample, channels)) {
    buf_free(&payload);
    candidate_free(&best);
    return 0;
  }
  Image metadata = {width, height, NULL};
  int ok = mk_file(&metadata, &best, file);
  candidate_free(&best);
  return ok;
}

static int qsw2_chunk(Buf *payload, const char tag[4], uint32_t flags,
                      const void *data, size_t size) {
  return buf_append(payload, tag, 4u) && buf_u32le(payload, flags) &&
         buf_u64le(payload, (uint64_t)size) && buf_append(payload, data, size);
}

static int qsw2_core_tag(const uint8_t tag[4]) {
  return memcmp(tag, "PIXL", 4u) == 0 || memcmp(tag, "ICCP", 4u) == 0 ||
         memcmp(tag, "CICP", 4u) == 0 || memcmp(tag, "MDCV", 4u) == 0 ||
         memcmp(tag, "CLLI", 4u) == 0;
}

static int hdr_metadata_valid(const HdrImage *image, uint32_t *chunk_count,
                              size_t *metadata_size) {
  int has_icc = image->icc_size != 0u;
  if ((!image->icc && has_icc) || (image->icc && !has_icc) ||
      image->sample_type != QSW2_SAMPLE_UINT ||
      image->color_authority > QSW2_COLOR_CICP_PREFERRED ||
      (image->has_cicp && image->full_range > 1u)) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid HDR metadata");
    return 0;
  }
  if ((image->wide.channels == 4u && image->alpha_mode != QSW2_ALPHA_STRAIGHT &&
       image->alpha_mode != QSW2_ALPHA_PREMULTIPLIED) ||
      (image->wide.channels != 4u && image->alpha_mode != QSW2_ALPHA_NONE)) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid HDR alpha association");
    return 0;
  }
  int authority_ok = (image->color_authority == QSW2_COLOR_UNSPECIFIED &&
                      !has_icc && !image->has_cicp) ||
                     (image->color_authority == QSW2_COLOR_ICC && has_icc &&
                      !image->has_cicp) ||
                     (image->color_authority == QSW2_COLOR_CICP && !has_icc &&
                      image->has_cicp) ||
                     ((image->color_authority == QSW2_COLOR_ICC_PREFERRED ||
                       image->color_authority == QSW2_COLOR_CICP_PREFERRED) &&
                      has_icc && image->has_cicp);
  if (!authority_ok) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT,
                   "HDR color authority does not match its metadata");
    return 0;
  }
  if (image->has_mastering_display &&
      image->max_luminance < image->min_luminance) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid HDR mastering luminance");
    return 0;
  }
  if (image->has_content_light && image->max_cll < image->max_fall) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid HDR content light");
    return 0;
  }
  if ((image->metadata_count && !image->metadata) ||
      image->metadata_count > QLIC_DEFAULT_MAX_CHUNKS - 1u) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid HDR metadata blocks");
    return 0;
  }
  size_t total = image->icc_size;
  if (image->has_cicp && !addok(total, 8u, &total))
    return 0;
  if (image->has_mastering_display && !addok(total, 24u, &total))
    return 0;
  if (image->has_content_light && !addok(total, 4u, &total))
    return 0;
  for (uint32_t index = 0; index < image->metadata_count; ++index) {
    const HdrMetadataBlock *block = image->metadata + index;
    if (block->reserved || (!block->data && block->size) ||
        qsw2_core_tag(block->tag)) {
      set_err_status(QLIC_CORE_BAD_ARGUMENT,
                     "invalid HDR ancillary metadata block");
      return 0;
    }
    if (!addok(total, block->size, &total))
      return 0;
  }
  if ((uint64_t)total > QLIC_DEFAULT_MAX_METADATA_BYTES) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: HDR metadata bytes");
    return 0;
  }
  *chunk_count = 1u + (uint32_t)has_icc + !!image->has_cicp +
                 !!image->has_mastering_display + !!image->has_content_light +
                 image->metadata_count;
  if (*chunk_count > QLIC_DEFAULT_MAX_CHUNKS) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: HDR metadata chunks");
    return 0;
  }
  *metadata_size = total;
  return 1;
}

int enc_hdr_mem(const HdrImage *image, Buf *file) {
  if (file)
    memset(file, 0, sizeof(*file));
  if (!image || !file) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid HDR encoder input");
    return 0;
  }
  uint32_t chunk_count = 0;
  size_t metadata_size = 0;
  if (!hdr_metadata_valid(image, &chunk_count, &metadata_size))
    return 0;
  Buf payload = {0};
  int ok = buf_append(&payload, "QSW2", 4u) && buf_u8(&payload, QSW2_VERSION) &&
           buf_u8(&payload, (uint8_t)image->sample_type) &&
           buf_u8(&payload, (uint8_t)image->wide.bits_per_sample) &&
           buf_u8(&payload, (uint8_t)image->wide.channels) &&
           buf_u8(&payload, (uint8_t)image->alpha_mode) &&
           buf_u8(&payload, (uint8_t)image->color_authority) &&
           buf_u16le(&payload, 0u) && buf_u32le(&payload, chunk_count) &&
           buf_u64le(&payload, (uint64_t)metadata_size) &&
           buf_u64le(&payload, 0u);
  if (ok && image->icc_size)
    ok = qsw2_chunk(&payload, "ICCP", 0u, image->icc, image->icc_size);
  if (ok && image->has_cicp) {
    uint8_t cicp[8];
    cicp[0] = (uint8_t)image->color_primaries;
    cicp[1] = (uint8_t)(image->color_primaries >> 8);
    cicp[2] = (uint8_t)image->transfer_characteristics;
    cicp[3] = (uint8_t)(image->transfer_characteristics >> 8);
    cicp[4] = (uint8_t)image->matrix_coefficients;
    cicp[5] = (uint8_t)(image->matrix_coefficients >> 8);
    cicp[6] = image->full_range;
    cicp[7] = 0u;
    ok = qsw2_chunk(&payload, "CICP", 0u, cicp, sizeof(cicp));
  }
  if (ok && image->has_mastering_display) {
    uint8_t mdcv[24];
    for (unsigned i = 0; i < 3u; ++i) {
      mdcv[i * 4u] = (uint8_t)image->primary_x[i];
      mdcv[i * 4u + 1u] = (uint8_t)(image->primary_x[i] >> 8);
      mdcv[i * 4u + 2u] = (uint8_t)image->primary_y[i];
      mdcv[i * 4u + 3u] = (uint8_t)(image->primary_y[i] >> 8);
    }
    mdcv[12] = (uint8_t)image->white_x;
    mdcv[13] = (uint8_t)(image->white_x >> 8);
    mdcv[14] = (uint8_t)image->white_y;
    mdcv[15] = (uint8_t)(image->white_y >> 8);
    wr32(mdcv + 16, image->max_luminance);
    wr32(mdcv + 20, image->min_luminance);
    ok = qsw2_chunk(&payload, "MDCV", 0u, mdcv, sizeof(mdcv));
  }
  if (ok && image->has_content_light) {
    uint8_t clli[4] = {(uint8_t)image->max_cll, (uint8_t)(image->max_cll >> 8),
                       (uint8_t)image->max_fall,
                       (uint8_t)(image->max_fall >> 8)};
    ok = qsw2_chunk(&payload, "CLLI", 0u, clli, sizeof(clli));
  }
  for (uint32_t index = 0; ok && index < image->metadata_count; ++index) {
    const HdrMetadataBlock *block = image->metadata + index;
    ok = qsw2_chunk(&payload, (const char *)block->tag, 0u, block->data,
                    block->size);
  }
  size_t pixel_chunk = payload.size;
  if (ok)
    ok = buf_append(&payload, "PIXL", 4u) &&
         buf_u32le(&payload, QSW2_CHUNK_CRITICAL) && buf_u64le(&payload, 0u);
  size_t pixel_start = payload.size;
  if (ok)
    ok = append_wide_payload(image->wide.pixels, image->wide.pixels_size,
                             image->wide.width, image->wide.height,
                             image->wide.stride, image->wide.channels,
                             image->wide.bits_per_sample, &payload);
  if (!ok) {
    buf_free(&payload);
    return 0;
  }
  size_t pixel_size = payload.size - pixel_start;
  wr64(payload.data + 24u, (uint64_t)pixel_size);
  wr64(payload.data + pixel_chunk + 8u, (uint64_t)pixel_size);
  Candidate best = {0};
  if (!try_store_owned(&best, &payload, MODE_HDR_WIDE, TRANSFORM_IDENTITY,
                       (int)image->wide.bits_per_sample,
                       image->wide.channels)) {
    buf_free(&payload);
    candidate_free(&best);
    return 0;
  }
  Image outer = {image->wide.width, image->wide.height, NULL};
  ok = mk_file(&outer, &best, file);
  candidate_free(&best);
  return ok;
}

typedef struct {
  const uint8_t *data;
  size_t size;
  uint32_t width;
  uint32_t height;
  int channels;
  unsigned threads;
  uint8_t *pixels;
  uint32_t decoded_width;
  uint32_t decoded_height;
  int decoded_channels;
  int error;
} WideSliceTask;

static void wide_slice_decode(void *context, unsigned index) {
  WideSliceTask *task = (WideSliceTask *)context + index;
  task->error = stream_decode_trusted_expected_threads(
      task->data, task->size, task->threads, task->width, task->height,
      task->channels, &task->pixels, &task->decoded_width,
      &task->decoded_height, &task->decoded_channels);
}

static int wide_slice_ok(const WideSliceTask *task) {
  return task->error == STREAM_OK && task->decoded_width == task->width &&
         task->decoded_height == task->height &&
         task->decoded_channels == task->channels;
}

static int wide_slice_error(const WideSliceTask *task) {
  if (task->error != STREAM_OK) {
    set_err_status(stream_failure_status(task->error, 1),
                   "wide byte-slice decode failed: %s",
                   stream_strerror(task->error));
  } else {
    set_err_status(QLIC_CORE_BAD_DATA,
                   "corrupt file: wide byte-slice layout mismatch");
  }
  return 0;
}

static int wide_payload_decode(const uint8_t *payload, size_t payload_size,
                               const QlicHeader *header, WideImage *out) {
  uint32_t bits = (uint32_t)header->index_bits;
  uint32_t channels = header->palette_count;
  uint32_t slice_count = (bits + 7u) / 8u;
  size_t table_size = (size_t)slice_count * 8u;
  size_t data_offset = QSW_HEADER_SIZE + table_size;
  if (payload_size < data_offset || memcmp(payload, "QSW1", 4u) != 0 ||
      payload[4] != QSW_METHOD_BYTE_SLICES || payload[5] != bits ||
      payload[6] != channels || payload[7] != slice_count ||
      rd32(payload + 12u) != 0u) {
    set_err("corrupt file: invalid QSW1 header");
    return 0;
  }
  size_t stream_end = data_offset;
  for (uint32_t index = 0; index < slice_count; ++index) {
    uint64_t length = rd64(payload + QSW_HEADER_SIZE + (size_t)index * 8u);
    if (!length || length > SIZE_MAX ||
        !addok(stream_end, (size_t)length, &stream_end) ||
        stream_end > payload_size) {
      set_err("corrupt file: invalid QSW1 slice length");
      return 0;
    }
  }
  if (stream_end != payload_size) {
    set_err("corrupt file: trailing QSW1 data");
    return 0;
  }
  size_t storage = bits <= 16u ? sizeof(uint16_t) : sizeof(uint32_t);
  size_t row_bytes = 0, output_size = 0;
  if (!mulok((size_t)header->width, (size_t)channels, &row_bytes) ||
      !mulok(row_bytes, storage, &row_bytes) ||
      !mulok(row_bytes, (size_t)header->height, &output_size)) {
    set_err("corrupt file: wide output size overflow");
    return 0;
  }
  void *pixels = calloc(1u, output_size);
  if (!pixels) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  size_t sample_count =
      (size_t)header->width * (size_t)header->height * (size_t)channels;
  uint32_t top_bits = bits - (slice_count - 1u) * 8u;
  uint8_t top_maximum = (uint8_t)((1u << top_bits) - 1u);
  WideSliceTask tasks[3];
  memset(tasks, 0, sizeof(tasks));
  size_t stream_offset = data_offset;
  unsigned outer_threads = g_threads ? g_threads : 1u;
  if (outer_threads > slice_count)
    outer_threads = slice_count;
  unsigned inner_threads = (g_threads ? g_threads : 1u) / outer_threads;
  if (!inner_threads)
    inner_threads = 1u;
  for (uint32_t index = 0; index < slice_count; ++index) {
    size_t length =
        (size_t)rd64(payload + QSW_HEADER_SIZE + (size_t)index * 8u);
    tasks[index].data = payload + stream_offset;
    tasks[index].size = length;
    tasks[index].width = header->width;
    tasks[index].height = header->height;
    tasks[index].channels = (int)channels;
    tasks[index].threads = inner_threads;
    stream_offset += length;
  }

  if (outer_threads > 1u) {
    qlic_parallel_for(slice_count, outer_threads, wide_slice_decode, tasks);
    for (uint32_t index = 0; index < slice_count; ++index) {
      if (!wide_slice_ok(&tasks[index])) {
        wide_slice_error(&tasks[index]);
        for (uint32_t cleanup = 0; cleanup < slice_count; ++cleanup)
          stream_free(tasks[cleanup].pixels);
        free(pixels);
        return 0;
      }
    }
  }

  for (uint32_t index = 0; index < slice_count; ++index) {
    if (outer_threads == 1u) {
      wide_slice_decode(tasks, index);
      if (!wide_slice_ok(&tasks[index])) {
        wide_slice_error(&tasks[index]);
        stream_free(tasks[index].pixels);
        free(pixels);
        return 0;
      }
    }
    uint8_t *slice = tasks[index].pixels;
    for (size_t sample = 0; sample < sample_count; ++sample) {
      uint32_t part = slice[sample];
      if (index + 1u == slice_count && part > top_maximum) {
        for (uint32_t cleanup = index; cleanup < slice_count; ++cleanup)
          stream_free(tasks[cleanup].pixels);
        free(pixels);
        set_err("corrupt file: wide sample exceeds declared precision");
        return 0;
      }
      if (storage == sizeof(uint16_t))
        ((uint16_t *)pixels)[sample] |= (uint16_t)(part << (index * 8u));
      else
        ((uint32_t *)pixels)[sample] |= part << (index * 8u);
    }
    stream_free(tasks[index].pixels);
    tasks[index].pixels = NULL;
  }
  uint32_t checksum = stream_crc32_wide(pixels, header->width, header->height,
                                        row_bytes, channels, bits);
  if (checksum != rd32(payload + 8u)) {
    free(pixels);
    set_err("corrupt file: wide pixel checksum mismatch");
    return 0;
  }
  out->width = header->width;
  out->height = header->height;
  out->channels = channels;
  out->bits_per_sample = bits;
  out->pixels = pixels;
  out->pixels_size = output_size;
  out->stride = row_bytes;
  return 1;
}

int dec_wide_qlic_limited(const uint8_t *data, size_t size, WideImage *out,
                          QlicHeader *header_out,
                          const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  if (!out) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid wide decoder output");
    return 0;
  }
  memset(out, 0, sizeof(*out));
  QlicHeader header = {0};
  if (!rd_head_limited(data, size, &header, limits))
    return 0;
  if (header.mode != MODE_NATIVE_WIDE) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "QLIC image is not a wide sample stream");
    return 0;
  }
  size_t compressed_size = (size_t)header.compressed_size;
  size_t body_size = size - QLIC_FOOTER_SIZE;
  size_t expected_size = 0;
  if (!addok(QLIC_HEADER_SIZE, compressed_size, &expected_size) ||
      expected_size != body_size || header.payload_size != compressed_size) {
    set_err("corrupt file: wide stream size does not match header");
    return 0;
  }
  int ok = wide_payload_decode(data + QLIC_HEADER_SIZE, compressed_size,
                               &header, out);
  if (ok && header_out)
    *header_out = header;
  return ok;
}

typedef struct {
  uint8_t tag[4];
  const uint8_t *data;
  size_t size;
} HdrMetadataView;

typedef struct {
  HdrInfo info;
  const uint8_t *pixels;
  size_t pixels_size;
  const uint8_t *icc;
  size_t icc_size;
  uint16_t primary_x[3];
  uint16_t primary_y[3];
  uint16_t white_x;
  uint16_t white_y;
  uint32_t max_luminance;
  uint32_t min_luminance;
  uint16_t max_cll;
  uint16_t max_fall;
  HdrMetadataView metadata[QLIC_DEFAULT_MAX_CHUNKS];
} HdrPayloadView;

static int qsw1_shape_valid(const uint8_t *payload, size_t payload_size,
                            const QlicHeader *header) {
  uint32_t bits = (uint32_t)header->index_bits;
  uint32_t channels = header->palette_count;
  uint32_t slice_count = (bits + 7u) / 8u;
  size_t data_offset = QSW_HEADER_SIZE + (size_t)slice_count * 8u;
  if (payload_size < data_offset || memcmp(payload, "QSW1", 4u) != 0 ||
      payload[4] != QSW_METHOD_BYTE_SLICES || payload[5] != bits ||
      payload[6] != channels || payload[7] != slice_count ||
      rd32(payload + 12u) != 0u)
    return 0;
  size_t end = data_offset;
  for (uint32_t index = 0; index < slice_count; ++index) {
    uint64_t length = rd64(payload + QSW_HEADER_SIZE + (size_t)index * 8u);
    if (!length || length > SIZE_MAX || !addok(end, (size_t)length, &end) ||
        end > payload_size)
      return 0;
  }
  return end == payload_size;
}

static int qsw2_color_valid(const HdrPayloadView *view) {
  int has_icc = view->info.has_icc != 0u;
  int has_cicp = view->info.has_cicp != 0u;
  switch (view->info.color_authority) {
  case QSW2_COLOR_UNSPECIFIED:
    return !has_icc && !has_cicp;
  case QSW2_COLOR_ICC:
    return has_icc && !has_cicp;
  case QSW2_COLOR_CICP:
    return !has_icc && has_cicp;
  case QSW2_COLOR_ICC_PREFERRED:
  case QSW2_COLOR_CICP_PREFERRED:
    return has_icc && has_cicp;
  default:
    return 0;
  }
}

static int qsw2_payload_parse(const uint8_t *payload, size_t payload_size,
                              const QlicHeader *header,
                              const QlicDecodeLimits *limits,
                              HdrPayloadView *view) {
  memset(view, 0, sizeof(*view));
  if (payload_size < QSW2_HEADER_SIZE || memcmp(payload, "QSW2", 4u) != 0 ||
      payload[4] != QSW2_VERSION || payload[5] != QSW2_SAMPLE_UINT ||
      payload[6] != (uint8_t)header->index_bits ||
      payload[7] != (uint8_t)header->palette_count || rd16(payload + 10u)) {
    set_err("corrupt file: invalid QSW2 header");
    return 0;
  }
  uint32_t alpha_mode = payload[8];
  uint32_t color_authority = payload[9];
  uint32_t chunk_count = rd32(payload + 12u);
  uint64_t declared_metadata = rd64(payload + 16u);
  uint64_t declared_pixels = rd64(payload + 24u);
  if (!chunk_count || chunk_count > limits->max_chunks ||
      chunk_count > QLIC_DEFAULT_MAX_CHUNKS) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: QSW2 chunks");
    return 0;
  }
  if (declared_metadata > limits->max_metadata_bytes) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "resource limit exceeded: HDR metadata bytes");
    return 0;
  }
  if (declared_pixels > SIZE_MAX || !declared_pixels) {
    set_err("corrupt file: invalid QSW2 pixel payload size");
    return 0;
  }
  if ((header->palette_count == 4u && alpha_mode != QSW2_ALPHA_STRAIGHT &&
       alpha_mode != QSW2_ALPHA_PREMULTIPLIED) ||
      (header->palette_count != 4u && alpha_mode != QSW2_ALPHA_NONE)) {
    set_err("corrupt file: invalid QSW2 alpha association");
    return 0;
  }

  size_t offset = QSW2_HEADER_SIZE;
  uint64_t metadata_size = 0;
  for (uint32_t chunk = 0; chunk < chunk_count; ++chunk) {
    size_t header_end = 0;
    if (!addok(offset, QSW2_CHUNK_HEADER_SIZE, &header_end) ||
        header_end > payload_size) {
      set_err("corrupt file: truncated QSW2 chunk header");
      return 0;
    }
    const uint8_t *tag = payload + offset;
    uint32_t flags = rd32(payload + offset + 4u);
    uint64_t length64 = rd64(payload + offset + 8u);
    if ((flags & ~(uint32_t)QSW2_CHUNK_CRITICAL) || length64 > SIZE_MAX) {
      set_err("corrupt file: invalid QSW2 chunk");
      return 0;
    }
    size_t end = 0;
    if (!addok(header_end, (size_t)length64, &end) || end > payload_size) {
      set_err("corrupt file: truncated QSW2 chunk payload");
      return 0;
    }
    const uint8_t *chunk_data = payload + header_end;
    size_t length = (size_t)length64;
    int known = 1;
    if (memcmp(tag, "PIXL", 4u) == 0) {
      if (view->pixels || flags != QSW2_CHUNK_CRITICAL ||
          length64 != declared_pixels) {
        set_err("corrupt file: invalid QSW2 PIXL chunk");
        return 0;
      }
      view->pixels = chunk_data;
      view->pixels_size = length;
    } else {
      if (metadata_size > UINT64_MAX - length64) {
        set_err("corrupt file: QSW2 metadata size overflow");
        return 0;
      }
      metadata_size += length64;
      if (metadata_size > limits->max_metadata_bytes) {
        set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                       "resource limit exceeded: HDR metadata bytes");
        return 0;
      }
      if (memcmp(tag, "ICCP", 4u) == 0) {
        if (view->info.has_icc || flags || !length) {
          set_err("corrupt file: invalid QSW2 ICCP chunk");
          return 0;
        }
        view->info.has_icc = 1u;
        view->icc = chunk_data;
        view->icc_size = length;
      } else if (memcmp(tag, "CICP", 4u) == 0) {
        if (view->info.has_cicp || flags || length != 8u || chunk_data[7] ||
            chunk_data[6] > 1u) {
          set_err("corrupt file: invalid QSW2 CICP chunk");
          return 0;
        }
        view->info.has_cicp = 1u;
        view->info.color_primaries = rd16(chunk_data);
        view->info.transfer_characteristics = rd16(chunk_data + 2u);
        view->info.matrix_coefficients = rd16(chunk_data + 4u);
        view->info.full_range = chunk_data[6];
      } else if (memcmp(tag, "MDCV", 4u) == 0) {
        if (view->info.has_mastering_display || flags || length != 24u) {
          set_err("corrupt file: invalid QSW2 MDCV chunk");
          return 0;
        }
        view->info.has_mastering_display = 1u;
        for (unsigned i = 0; i < 3u; ++i) {
          view->primary_x[i] = rd16(chunk_data + i * 4u);
          view->primary_y[i] = rd16(chunk_data + i * 4u + 2u);
        }
        view->white_x = rd16(chunk_data + 12u);
        view->white_y = rd16(chunk_data + 14u);
        view->max_luminance = rd32(chunk_data + 16u);
        view->min_luminance = rd32(chunk_data + 20u);
        if (view->max_luminance < view->min_luminance) {
          set_err("corrupt file: invalid QSW2 mastering luminance");
          return 0;
        }
      } else if (memcmp(tag, "CLLI", 4u) == 0) {
        if (view->info.has_content_light || flags || length != 4u) {
          set_err("corrupt file: invalid QSW2 CLLI chunk");
          return 0;
        }
        view->info.has_content_light = 1u;
        view->max_cll = rd16(chunk_data);
        view->max_fall = rd16(chunk_data + 2u);
        if (view->max_cll < view->max_fall) {
          set_err("corrupt file: invalid QSW2 content light metadata");
          return 0;
        }
      } else {
        known = 0;
      }
      if (!known && (flags & QSW2_CHUNK_CRITICAL)) {
        set_err("unsupported critical QSW2 chunk");
        return 0;
      }
      if (!known) {
        HdrMetadataView *metadata =
            view->metadata + view->info.metadata_count++;
        memcpy(metadata->tag, tag, sizeof(metadata->tag));
        metadata->data = chunk_data;
        metadata->size = length;
      }
    }
    offset = end;
  }
  view->info.channels = header->palette_count;
  view->info.bits_per_sample = (uint32_t)header->index_bits;
  view->info.sample_type = QSW2_SAMPLE_UINT;
  view->info.alpha_mode = alpha_mode;
  view->info.color_authority = color_authority;
  if (offset != payload_size || metadata_size != declared_metadata ||
      !view->pixels || !qsw2_color_valid(view) ||
      !qsw1_shape_valid(view->pixels, view->pixels_size, header)) {
    set_err("corrupt file: inconsistent QSW2 payload");
    return 0;
  }
  return 1;
}

static int hdr_outer_view(const uint8_t *data, size_t size,
                          const QlicDecodeLimits *limits, QlicHeader *header,
                          const uint8_t **payload, size_t *payload_size) {
  if (!rd_head_limited(data, size, header, limits))
    return 0;
  if (header->mode != MODE_HDR_WIDE) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "QLIC image is not a self-describing HDR stream");
    return 0;
  }
  size_t compressed_size = (size_t)header->compressed_size;
  size_t body_size = size - QLIC_FOOTER_SIZE;
  size_t expected_size = 0;
  if (!addok(QLIC_HEADER_SIZE, compressed_size, &expected_size) ||
      expected_size != body_size || header->payload_size != compressed_size) {
    set_err("corrupt file: HDR stream size does not match header");
    return 0;
  }
  *payload = data + QLIC_HEADER_SIZE;
  *payload_size = compressed_size;
  return 1;
}

int hdr_qlic_info_limited(const uint8_t *data, size_t size, HdrInfo *out,
                          QlicHeader *header_out,
                          const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  if (!out) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid HDR info output");
    return 0;
  }
  memset(out, 0, sizeof(*out));
  QlicHeader header = {0};
  const uint8_t *payload = NULL;
  size_t payload_size = 0;
  HdrPayloadView view;
  if (!hdr_outer_view(data, size, limits, &header, &payload, &payload_size) ||
      !qsw2_payload_parse(payload, payload_size, &header, limits, &view))
    return 0;
  *out = view.info;
  if (header_out)
    *header_out = header;
  return 1;
}

int dec_hdr_qlic_limited(const uint8_t *data, size_t size, HdrImage *out,
                         QlicHeader *header_out,
                         const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  if (!out) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid HDR decoder output");
    return 0;
  }
  memset(out, 0, sizeof(*out));
  QlicHeader header = {0};
  const uint8_t *payload = NULL;
  size_t payload_size = 0;
  HdrPayloadView view;
  if (!hdr_outer_view(data, size, limits, &header, &payload, &payload_size) ||
      !qsw2_payload_parse(payload, payload_size, &header, limits, &view) ||
      !wide_payload_decode(view.pixels, view.pixels_size, &header,
                           &out->wide)) {
    hdr_image_free(out);
    return 0;
  }
  if (view.icc_size) {
    out->icc = (uint8_t *)xmalloc(view.icc_size);
    if (!out->icc) {
      hdr_image_free(out);
      return 0;
    }
    memcpy(out->icc, view.icc, view.icc_size);
    out->icc_size = view.icc_size;
  }
  if (view.info.metadata_count) {
    size_t table_size = 0;
    if (!mulok((size_t)view.info.metadata_count,
               sizeof(*out->metadata), &table_size)) {
      hdr_image_free(out);
      return 0;
    }
    out->metadata = (HdrMetadataBlock *)calloc(1u, table_size);
    if (!out->metadata) {
      set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
      hdr_image_free(out);
      return 0;
    }
    out->metadata_count = view.info.metadata_count;
    for (uint32_t index = 0; index < out->metadata_count; ++index) {
      const HdrMetadataView *source = view.metadata + index;
      HdrMetadataBlock *destination = out->metadata + index;
      memcpy(destination->tag, source->tag, sizeof(destination->tag));
      if (source->size) {
        destination->data = (uint8_t *)xmalloc(source->size);
        if (!destination->data) {
          hdr_image_free(out);
          return 0;
        }
        memcpy(destination->data, source->data, source->size);
        destination->size = source->size;
      }
    }
  }
  out->sample_type = view.info.sample_type;
  out->alpha_mode = view.info.alpha_mode;
  out->color_authority = view.info.color_authority;
  out->has_cicp = view.info.has_cicp;
  out->color_primaries = view.info.color_primaries;
  out->transfer_characteristics = view.info.transfer_characteristics;
  out->matrix_coefficients = view.info.matrix_coefficients;
  out->full_range = view.info.full_range;
  out->has_mastering_display = view.info.has_mastering_display;
  memcpy(out->primary_x, view.primary_x, sizeof(out->primary_x));
  memcpy(out->primary_y, view.primary_y, sizeof(out->primary_y));
  out->white_x = view.white_x;
  out->white_y = view.white_y;
  out->max_luminance = view.max_luminance;
  out->min_luminance = view.min_luminance;
  out->has_content_light = view.info.has_content_light;
  out->max_cll = view.max_cll;
  out->max_fall = view.max_fall;
  if (header_out)
    *header_out = header;
  return 1;
}

static int dec_anim_qlic_limited(const uint8_t *data, size_t size, Anim *out,
                                 QlicHeader *header_out,
                                 const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  if (!out) {
    set_err("invalid decoder output");
    return 0;
  }
  QlicHeader h = {0};
  if (!rd_head_limited(data, size, &h, limits))
    return 0;
  if (h.mode != MODE_ANIM) {
    set_err("not an animated QLIC file");
    return 0;
  }
  size_t comp_size = (size_t)h.compressed_size;
  size_t need;
  if (!addok(QLIC_HEADER_SIZE, comp_size, &need))
    return 0;
  size_t body_size = size - QLIC_FOOTER_SIZE;
  if (need != body_size) {
    set_err("corrupt file: file size does not match header");
    return 0;
  }
  Buf payload = {0};
  if (!unzip(data + QLIC_HEADER_SIZE, comp_size, h.codec, h.payload_size,
             &payload, limits)) {
    unzip_free(&payload, h.codec);
    return 0;
  }
  int ok = dec_anim_payload(&payload, &h, out, limits);
  unzip_free(&payload, h.codec);
  if (ok && header_out)
    *header_out = h;
  return ok;
}

#ifndef QLIC_NO_MAIN
static int dec_anim_qlic(const uint8_t *data, size_t size, Anim *out,
                         QlicHeader *header_out) {
  return dec_anim_qlic_limited(data, size, out, header_out, NULL);
}
#endif

int dec_any_qlic_limited(const uint8_t *data, size_t size, Anim *out,
                         QlicHeader *header_out,
                         const QlicDecodeLimits *decode_limit) {
  const QlicDecodeLimits *limits = decode_limits(decode_limit);
  if (!out) {
    set_err("invalid decoder output");
    return 0;
  }
  if (data && size > 12u && data[12] == MODE_ANIM)
    return dec_anim_qlic_limited(data, size, out, header_out, limits);
  QlicHeader h = {0};
  Image image = {0};
  if (!dec_qlic_limited(data, size, &image, &h, limits))
    return 0;
  out->frames = (AnimFrame *)calloc(1, sizeof(*out->frames));
  if (!out->frames) {
    image_free(&image);
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  out->frames[0].image = image;
  out->width = image.width;
  out->height = image.height;
  out->count = 1;
  out->loop_count = 0;
  out->frames[0].delay_ms = 0;
  if (header_out)
    *header_out = h;
  return 1;
}

int dec_any_qlic(const uint8_t *data, size_t size, Anim *out,
                 QlicHeader *header_out) {
  return dec_any_qlic_limited(data, size, out, header_out, NULL);
}

#ifndef QLIC_NO_MAIN
static double now_s(void) {
  static LARGE_INTEGER freq;
  static int init = 0;
  LARGE_INTEGER v;
  if (!init) {
    QueryPerformanceFrequency(&freq);
    init = 1;
  }
  QueryPerformanceCounter(&v);
  return (double)v.QuadPart / (double)freq.QuadPart;
}

static int enc_file(const Image *im, const wchar_t *out_path, Candidate *chosen,
                    uint64_t *written) {
  Candidate best = {0};
  Buf file = {0};
  if (!enc_mem(im, &file, &best)) {
    candidate_free(&best);
    buf_free(&file);
    return 0;
  }
  int ok = wr_file(out_path, file.data, file.size);
  if (ok && written)
    *written = (uint64_t)file.size;
  if (ok && chosen) {
    *chosen = best;
    memset(&best, 0, sizeof(best));
  }
  candidate_free(&best);
  buf_free(&file);
  return ok;
}
#endif

int enc_mem(const Image *im, Buf *file, Candidate *chosen) {
  Candidate best;
  memset(&best, 0, sizeof(best));
  if (!enc_best(im, &best)) {
    candidate_free(&best);
    return 0;
  }
#if defined(_WIN32) || defined(QLIC_HAVE_WIMLIB)
  if (!try_planar_med_lzms(&best, im)) {
    candidate_free(&best);
    return 0;
  }
#endif
  if (!mk_file(im, &best, file)) {
    candidate_free(&best);
    return 0;
  }
  if (chosen) {
    *chosen = best;
    memset(&best, 0, sizeof(best));
  }
  candidate_free(&best);
  return 1;
}

typedef struct {
  const Image *image;
  Image owned;
  Buf frame;
  uint32_t type;
  uint32_t x;
  uint32_t y;
  uint32_t source_x;
  uint32_t source_y;
  uint32_t width;
  uint32_t height;
  uint32_t clear;
  unsigned inner_threads;
  int error;
  int ok;
} AnimEncTask;

typedef struct {
  int64_t dx;
  int64_t dy;
  uint32_t count;
} AnimMotionVote;

typedef struct {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
  uint32_t saved;
  int64_t dx;
  int64_t dy;
} AnimCopyCandidate;

static int anim_changed_rect(const Image *previous, const Image *current,
                             uint32_t *rx, uint32_t *ry, uint32_t *rw,
                             uint32_t *rh, uint64_t *changed) {
  uint32_t min_x = current->width;
  uint32_t min_y = current->height;
  uint32_t max_x = 0;
  uint32_t max_y = 0;
  uint64_t count = 0;
  size_t row_bytes = (size_t)current->width * 4u;
  for (uint32_t y = 0; y < current->height; ++y) {
    const uint8_t *a = previous->rgba + (size_t)y * row_bytes;
    const uint8_t *b = current->rgba + (size_t)y * row_bytes;
    if (memcmp(a, b, row_bytes) == 0)
      continue;
    for (uint32_t x = 0; x < current->width; ++x) {
      if (memcmp(a + (size_t)x * 4u, b + (size_t)x * 4u, 4u) == 0)
        continue;
      if (x < min_x)
        min_x = x;
      if (x > max_x)
        max_x = x;
      if (y < min_y)
        min_y = y;
      max_y = y;
      ++count;
    }
  }
  if (min_y == current->height)
    return 0;
  *rx = min_x;
  *ry = min_y;
  *rw = max_x - min_x + 1u;
  *rh = max_y - min_y + 1u;
  *changed = count;
  return 1;
}

static MapEntry *anim_hash_slot(MapEntry *map, size_t mask, uint32_t key) {
  size_t pos = (size_t)hkey(key) & mask;
  while (map[pos].used && map[pos].key != key)
    pos = (pos + 1u) & mask;
  return &map[pos];
}

static uint32_t anim_patch_key(const uint8_t *pixel, size_t stride) {
  uint32_t hash = UINT32_C(2166136261);
  for (uint32_t y = 0; y < 2u; ++y) {
    for (uint32_t x = 0; x < 8u; ++x) {
      hash ^= pixel[(size_t)y * stride + x];
      hash *= UINT32_C(16777619);
    }
  }
  return hash;
}

static uint32_t anim_rect_sum(const uint32_t *prefix, size_t stride, uint32_t x,
                              uint32_t y, uint32_t width, uint32_t height) {
  size_t x0 = x, x1 = (size_t)x + width;
  size_t y0 = y, y1 = (size_t)y + height;
  return prefix[y1 * stride + x1] - prefix[y0 * stride + x1] -
         prefix[y1 * stride + x0] + prefix[y0 * stride + x0];
}

static int anim_copy_better(const AnimCopyCandidate *a,
                            const AnimCopyCandidate *b) {
  uint64_t area_a = (uint64_t)a->width * a->height;
  uint64_t area_b = (uint64_t)b->width * b->height;
  return a->saved > b->saved || (a->saved == b->saved && area_a < area_b);
}

static void anim_copy_add(AnimCopyCandidate *best, uint32_t *count,
                          const AnimCopyCandidate *candidate) {
  if (!candidate->saved || !candidate->width || !candidate->height)
    return;
  uint32_t pos = *count;
  if (pos < 16u) {
    ++*count;
  } else {
    pos = 15u;
    if (!anim_copy_better(candidate, &best[pos]))
      return;
  }
  while (pos && anim_copy_better(candidate, &best[pos - 1u])) {
    if (pos < 16u)
      best[pos] = best[pos - 1u];
    --pos;
  }
  best[pos] = *candidate;
}

static void anim_motion_rects(const Image *previous, const Image *current,
                              uint32_t bx, uint32_t by, uint32_t bw,
                              uint32_t bh, int64_t dx, int64_t dy,
                              const uint32_t *prefix, AnimCopyCandidate *best,
                              uint32_t *best_count, uint32_t *heights,
                              uint32_t *stack_height, uint32_t *stack_start) {
  size_t image_stride = (size_t)current->width * 4u;
  size_t prefix_stride = (size_t)bw + 1u;
  memset(heights, 0, (size_t)bw * sizeof(*heights));
  for (uint32_t yy = 0; yy < bh; ++yy) {
    uint32_t y = by + yy;
    for (uint32_t xx = 0; xx < bw; ++xx) {
      uint32_t x = bx + xx;
      int64_t source_x = (int64_t)x - dx;
      int64_t source_y = (int64_t)y - dy;
      int match =
          source_x >= 0 && source_y >= 0 &&
          (uint64_t)source_x < previous->width &&
          (uint64_t)source_y < previous->height &&
          memcmp(current->rgba + (size_t)y * image_stride + (size_t)x * 4u,
                 previous->rgba + (size_t)source_y * image_stride +
                     (size_t)source_x * 4u,
                 4u) == 0;
      heights[xx] = match ? heights[xx] + 1u : 0u;
    }
    uint32_t top = 0;
    for (uint32_t xx = 0; xx <= bw; ++xx) {
      uint32_t height = xx < bw ? heights[xx] : 0u;
      uint32_t start = xx;
      while (top && stack_height[top - 1u] > height) {
        --top;
        uint32_t candidate_height = stack_height[top];
        start = stack_start[top];
        AnimCopyCandidate candidate = {
            bx + start, by + yy + 1u - candidate_height,
            xx - start, candidate_height,
            0,          dx,
            dy};
        candidate.saved = anim_rect_sum(prefix, prefix_stride, start,
                                        yy + 1u - candidate_height,
                                        candidate.width, candidate.height);
        anim_copy_add(best, best_count, &candidate);
      }
      if (height && (!top || stack_height[top - 1u] < height)) {
        stack_height[top] = height;
        stack_start[top] = start;
        ++top;
      }
    }
  }
}

static int anim_verify_move(const Image *previous, const Image *current,
                            const AnimCopyCandidate *candidate,
                            AnimEncTask *task) {
  int64_t source_x64 = (int64_t)candidate->x - candidate->dx;
  int64_t source_y64 = (int64_t)candidate->y - candidate->dy;
  if (source_x64 < 0 || source_y64 < 0 ||
      (uint64_t)source_x64 + candidate->width > previous->width ||
      (uint64_t)source_y64 + candidate->height > previous->height)
    return 0;
  uint32_t source_x = (uint32_t)source_x64;
  uint32_t source_y = (uint32_t)source_y64;
  uint32_t clear = 0;
  int have_clear = 0;
  size_t stride = (size_t)current->width * 4u;
  for (uint32_t yy = 0; yy < candidate->height && !have_clear; ++yy) {
    uint32_t y = source_y + yy;
    for (uint32_t xx = 0; xx < candidate->width; ++xx) {
      uint32_t x = source_x + xx;
      if (x >= candidate->x && x - candidate->x < candidate->width &&
          y >= candidate->y && y - candidate->y < candidate->height)
        continue;
      clear = ckey(current->rgba + (size_t)y * stride + (size_t)x * 4u);
      have_clear = 1;
      break;
    }
  }
  if (!have_clear)
    return 0;
  uint8_t clear_bytes[4] = {(uint8_t)clear, (uint8_t)(clear >> 8),
                            (uint8_t)(clear >> 16), (uint8_t)(clear >> 24)};
  uint32_t verify_x = source_x < candidate->x ? source_x : candidate->x;
  uint32_t verify_y = source_y < candidate->y ? source_y : candidate->y;
  uint32_t source_right = source_x + candidate->width;
  uint32_t source_bottom = source_y + candidate->height;
  uint32_t destination_right = candidate->x + candidate->width;
  uint32_t destination_bottom = candidate->y + candidate->height;
  uint32_t verify_right =
      source_right > destination_right ? source_right : destination_right;
  uint32_t verify_bottom =
      source_bottom > destination_bottom ? source_bottom : destination_bottom;
  for (uint32_t y = verify_y; y < verify_bottom; ++y) {
    for (uint32_t x = verify_x; x < verify_right; ++x) {
      const uint8_t *predicted =
          previous->rgba + (size_t)y * stride + (size_t)x * 4u;
      if (x >= candidate->x && x - candidate->x < candidate->width &&
          y >= candidate->y && y - candidate->y < candidate->height) {
        uint32_t sx = source_x + x - candidate->x;
        uint32_t sy = source_y + y - candidate->y;
        predicted = previous->rgba + (size_t)sy * stride + (size_t)sx * 4u;
      } else if (x >= source_x && x - source_x < candidate->width &&
                 y >= source_y && y - source_y < candidate->height) {
        predicted = clear_bytes;
      }
      if (memcmp(predicted, current->rgba + (size_t)y * stride + (size_t)x * 4u,
                 4u) != 0)
        return 0;
    }
  }
  task->type = ANIM_FRAME_MOVE;
  task->source_x = source_x;
  task->source_y = source_y;
  task->x = candidate->x;
  task->y = candidate->y;
  task->width = candidate->width;
  task->height = candidate->height;
  task->clear = clear;
  task->image = NULL;
  return 1;
}

static int anim_find_move(const Image *previous, const Image *current,
                          uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh,
                          uint64_t changed, AnimEncTask *task) {
  uint64_t pixels64 = (uint64_t)bw * bh;
  /* failed motion trials fall back to the rectangle path, so their cost stays
   * bounded */
  if (pixels64 < 64u || pixels64 > 262144u || pixels64 > UINT32_MAX ||
      current->width > INT32_MAX || current->height > INT32_MAX)
    return 0;
  size_t pixels = (size_t)pixels64;
  size_t map_need = 0;
  if (!mulok(pixels, 2u, &map_need))
    return 0;
  size_t map_cap = np2(map_need);
  if (map_cap < map_need)
    return 0;
  MapEntry *map = (MapEntry *)calloc(map_cap, sizeof(*map));
  size_t prefix_count = 0;
  if (!mulok((size_t)bw + 1u, (size_t)bh + 1u, &prefix_count)) {
    free(map);
    return 0;
  }
  uint32_t *prefix = (uint32_t *)calloc(prefix_count, sizeof(*prefix));
  uint32_t *work = (uint32_t *)malloc((size_t)bw * 3u * sizeof(*work));
  if (!map || !prefix || !work) {
    free(work);
    free(prefix);
    free(map);
    return 0;
  }
  size_t stride = (size_t)current->width * 4u;
  for (uint32_t yy = 0; yy < bh; ++yy) {
    uint32_t row_sum = 0;
    for (uint32_t xx = 0; xx < bw; ++xx) {
      const uint8_t *old_pixel =
          previous->rgba + (size_t)(by + yy) * stride + (size_t)(bx + xx) * 4u;
      const uint8_t *new_pixel =
          current->rgba + (size_t)(by + yy) * stride + (size_t)(bx + xx) * 4u;
      row_sum += memcmp(old_pixel, new_pixel, 4u) != 0;
      prefix[(size_t)(yy + 1u) * ((size_t)bw + 1u) + xx + 1u] =
          prefix[(size_t)yy * ((size_t)bw + 1u) + xx + 1u] + row_sum;
    }
  }
  for (uint32_t yy = 0; yy + 1u < bh; ++yy) {
    for (uint32_t xx = 0; xx + 1u < bw; ++xx) {
      const uint8_t *old_pixel =
          previous->rgba + (size_t)(by + yy) * stride + (size_t)(bx + xx) * 4u;
      uint32_t key = anim_patch_key(old_pixel, stride);
      MapEntry *entry = anim_hash_slot(map, map_cap - 1u, key);
      uint32_t position = yy * bw + xx;
      if (!entry->used) {
        entry->used = 1;
        entry->key = key;
        entry->value = position;
      } else {
        entry->value = UINT32_MAX;
      }
    }
  }
  uint32_t step = 1;
  while ((((uint64_t)bw + step - 1u) / step) *
             (((uint64_t)bh + step - 1u) / step) >
         8192u)
    ++step;
  AnimMotionVote votes[16] = {0};
  uint32_t vote_count = 0;
  for (uint32_t yy = 0; yy + 1u < bh; yy += step) {
    for (uint32_t xx = 0; xx + 1u < bw; xx += step) {
      const uint8_t *old_pixel =
          previous->rgba + (size_t)(by + yy) * stride + (size_t)(bx + xx) * 4u;
      const uint8_t *new_pixel =
          current->rgba + (size_t)(by + yy) * stride + (size_t)(bx + xx) * 4u;
      if (memcmp(old_pixel, new_pixel, 4u) == 0)
        continue;
      uint32_t key = anim_patch_key(new_pixel, stride);
      MapEntry *entry = anim_hash_slot(map, map_cap - 1u, key);
      if (!entry->used || entry->value == UINT32_MAX)
        continue;
      uint32_t source_x = entry->value % bw;
      uint32_t source_y = entry->value / bw;
      const uint8_t *source = previous->rgba +
                              (size_t)(by + source_y) * stride +
                              (size_t)(bx + source_x) * 4u;
      if (memcmp(new_pixel, source, 8u) != 0 ||
          memcmp(new_pixel + stride, source + stride, 8u) != 0)
        continue;
      int64_t dx = (int64_t)xx - source_x;
      int64_t dy = (int64_t)yy - source_y;
      if (!dx && !dy)
        continue;
      uint32_t slot = 0;
      while (slot < vote_count &&
             (votes[slot].dx != dx || votes[slot].dy != dy))
        ++slot;
      if (slot == vote_count) {
        if (vote_count == 16u)
          continue;
        votes[slot].dx = dx;
        votes[slot].dy = dy;
        ++vote_count;
      }
      ++votes[slot].count;
    }
  }
  AnimCopyCandidate candidates[16] = {0};
  uint32_t candidate_count = 0;
  uint32_t *heights = work;
  uint32_t *stack_height = work + bw;
  uint32_t *stack_start = work + (size_t)bw * 2u;
  for (uint32_t i = 0; i < vote_count; ++i) {
    if (votes[i].count < 4u)
      continue;
    anim_motion_rects(previous, current, bx, by, bw, bh, votes[i].dx,
                      votes[i].dy, prefix, candidates, &candidate_count,
                      heights, stack_height, stack_start);
  }
  int found = 0;
  for (uint32_t i = 0; i < candidate_count; ++i) {
    if ((uint64_t)candidates[i].saved * 2u < changed)
      continue;
    /* Exact verification rejects weak votes and hash collisions. */
    if (anim_verify_move(previous, current, &candidates[i], task))
      found = 1;
    if (found)
      break;
  }
  free(work);
  free(prefix);
  free(map);
  return found;
}

static int anim_crop(const Image *source, uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h, Image *crop) {
  size_t row_bytes = 0, bytes = 0;
  if (!mulok((size_t)w, 4u, &row_bytes) || !mulok(row_bytes, (size_t)h, &bytes))
    return 0;
  uint8_t *rgba = (uint8_t *)xmalloc(bytes);
  if (!rgba)
    return 0;
  size_t source_stride = (size_t)source->width * 4u;
  for (uint32_t yy = 0; yy < h; ++yy)
    memcpy(rgba + (size_t)yy * row_bytes,
           source->rgba + (size_t)(y + yy) * source_stride + (size_t)x * 4u,
           row_bytes);
  crop->width = w;
  crop->height = h;
  crop->rgba = rgba;
  return 1;
}

static void anim_enc_one(AnimEncTask *task) {
  if (task->type == ANIM_FRAME_DUPLICATE || task->type == ANIM_FRAME_MOVE) {
    task->ok = 1;
    return;
  }
  unsigned previous = g_threads;
  clear_err();
  qlic_core_set_thread_count(task->inner_threads);
  task->ok = enc_mem(task->image, &task->frame, NULL);
  if (!task->ok) {
    QlicCoreStatus status = qlic_core_status();
    if (status == QLIC_CORE_OUT_OF_MEMORY)
      task->error = ANIM_TASK_ALLOC;
    else if (status == QLIC_CORE_LIMIT_EXCEEDED)
      task->error = ANIM_TASK_LIMIT;
  }
  qlic_core_set_thread_count(previous);
}

static void anim_enc_item(void *context, unsigned index) {
  anim_enc_one(&((AnimEncTask *)context)[index]);
}

int enc_anim_mem(const Anim *anim, Buf *file, Candidate *chosen) {
  if (!anim || !anim->frames || anim->count == 0) {
    set_err("empty animation");
    return 0;
  }
  if (!anim->width || !anim->height || anim->count > 100000u) {
    set_err("invalid animation");
    return 0;
  }
  for (uint32_t i = 0; i < anim->count; ++i) {
    const Image *image = &anim->frames[i].image;
    if (!image->rgba || !image->width || !image->height ||
        image->width != anim->width || image->height != anim->height) {
      set_err("invalid animation frame dimensions");
      return 0;
    }
  }
  unsigned budget = g_threads ? g_threads : 1u;
  unsigned workers = budget;
  if (workers > anim->count)
    workers = anim->count;
  uint32_t capacity = workers > anim->count / 2u ? anim->count : workers * 2u;
  /* two frame slots per worker keep parallelism without retaining the whole
   * animation */
  AnimEncTask *tasks = (AnimEncTask *)calloc(capacity, sizeof(*tasks));
  if (!tasks) {
    set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
    return 0;
  }
  Buf payload = {0};
  if (!buf_append(&payload, "QAN2", 4) || !buf_u32le(&payload, anim->count) ||
      !buf_u32le(&payload, anim->loop_count)) {
    free(tasks);
    buf_free(&payload);
    return 0;
  }
  size_t last_key_size = SIZE_MAX;
  for (uint32_t base = 0; base < anim->count; base += capacity) {
    uint32_t count = anim->count - base;
    if (count > capacity)
      count = capacity;
    memset(tasks, 0, (size_t)count * sizeof(*tasks));
    int ok = 1;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t index = base + i;
      const Image *current = &anim->frames[index].image;
      tasks[i].type = ANIM_FRAME_KEY;
      tasks[i].image = current;
      if (index) {
        uint32_t x = 0, y = 0, w = 0, h = 0;
        uint64_t changed = 0;
        const Image *previous = &anim->frames[index - 1u].image;
        if (!anim_changed_rect(previous, current, &x, &y, &w, &h, &changed)) {
          tasks[i].type = ANIM_FRAME_DUPLICATE;
          tasks[i].image = NULL;
        } else if (!anim_find_move(previous, current, x, y, w, h, changed,
                                   &tasks[i])) {
          uint64_t rect_pixels = (uint64_t)w * h;
          uint64_t frame_pixels = (uint64_t)anim->width * anim->height;
          if (rect_pixels * 2u <= frame_pixels) {
            if (!anim_crop(current, x, y, w, h, &tasks[i].owned)) {
              ok = 0;
              break;
            }
            tasks[i].type = ANIM_FRAME_RECT;
            tasks[i].x = x;
            tasks[i].y = y;
            tasks[i].image = &tasks[i].owned;
          }
        }
      }
    }
    /* duplicates and moves do not divide the worker budget */
    unsigned active = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (tasks[i].type == ANIM_FRAME_KEY || tasks[i].type == ANIM_FRAME_RECT)
        ++active;
    }
    unsigned outer = active < workers ? active : workers;
    if (!outer)
      outer = 1u;
    unsigned inner = active ? budget / outer : 1u;
    unsigned extra = active ? budget % outer : 0u;
    unsigned slot = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (tasks[i].type == ANIM_FRAME_KEY || tasks[i].type == ANIM_FRAME_RECT)
        tasks[i].inner_threads = inner + (slot++ < extra);
      else
        tasks[i].inner_threads = 1u;
    }
    if (ok && outer <= 1u) {
      for (uint32_t i = 0; i < count; ++i)
        anim_enc_one(&tasks[i]);
    } else if (ok) {
      qlic_parallel_for(count, outer, anim_enc_item, tasks);
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (ok && !tasks[i].ok) {
        if (tasks[i].error == ANIM_TASK_ALLOC)
          set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
        else if (tasks[i].error == ANIM_TASK_LIMIT)
          set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                         "resource limit exceeded: animation frame");
        else
          set_err("animation frame encode failed");
        ok = 0;
        break;
      }
    }
    for (uint32_t i = 0; i < count && ok; ++i) {
      uint32_t index = base + i;
      Buf *frame = &tasks[i].frame;
      if (tasks[i].type == ANIM_FRAME_RECT && last_key_size != SIZE_MAX &&
          frame->size + 32u >= last_key_size + 16u) {
        AnimEncTask full = {0};
        full.image = &anim->frames[index].image;
        full.type = ANIM_FRAME_KEY;
        full.inner_threads = budget;
        anim_enc_one(&full);
        if (!full.ok) {
          if (full.error == ANIM_TASK_ALLOC)
            set_err_status(QLIC_CORE_OUT_OF_MEMORY, "out of memory");
          else if (full.error == ANIM_TASK_LIMIT)
            set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                           "resource limit exceeded: animation frame");
          else
            set_err("animation frame encode failed");
          buf_free(&full.frame);
          ok = 0;
          break;
        }
        if (full.frame.size + 16u < frame->size + 32u) {
          buf_free(frame);
          *frame = full.frame;
          memset(&full.frame, 0, sizeof(full.frame));
          tasks[i].type = ANIM_FRAME_KEY;
        }
        buf_free(&full.frame);
      }
      if (!buf_u32le(&payload, anim->frames[index].delay_ms
                                   ? anim->frames[index].delay_ms
                                   : 100u) ||
          !buf_u32le(&payload, tasks[i].type))
        ok = 0;
      if (ok && tasks[i].type == ANIM_FRAME_KEY &&
          (!buf_u64le(&payload, (uint64_t)frame->size) ||
           !buf_append(&payload, frame->data, frame->size)))
        ok = 0;
      if (ok && tasks[i].type == ANIM_FRAME_RECT &&
          (!buf_u32le(&payload, tasks[i].x) ||
           !buf_u32le(&payload, tasks[i].y) ||
           !buf_u32le(&payload, tasks[i].owned.width) ||
           !buf_u32le(&payload, tasks[i].owned.height) ||
           !buf_u64le(&payload, (uint64_t)frame->size) ||
           !buf_append(&payload, frame->data, frame->size)))
        ok = 0;
      if (ok && tasks[i].type == ANIM_FRAME_MOVE &&
          (!buf_u32le(&payload, tasks[i].source_x) ||
           !buf_u32le(&payload, tasks[i].source_y) ||
           !buf_u32le(&payload, tasks[i].x) ||
           !buf_u32le(&payload, tasks[i].y) ||
           !buf_u32le(&payload, tasks[i].width) ||
           !buf_u32le(&payload, tasks[i].height) ||
           !buf_u32le(&payload, tasks[i].clear)))
        ok = 0;
      if (ok && tasks[i].type == ANIM_FRAME_KEY)
        last_key_size = frame->size;
    }
    for (uint32_t i = 0; i < count; ++i) {
      buf_free(&tasks[i].frame);
      image_free(&tasks[i].owned);
    }
    if (!ok) {
      free(tasks);
      buf_free(&payload);
      return 0;
    }
  }
  free(tasks);

  Candidate best;
  memset(&best, 0, sizeof(best));
  int codec = CODEC_STORE;
  if (!try_pay(&best, &payload, NULL, MODE_ANIM, TRANSFORM_IDENTITY, 0,
               anim->count, &codec, 1)) {
    buf_free(&payload);
    candidate_free(&best);
    return 0;
  }
  Image pseudo = {anim->width, anim->height, NULL};
  int ok = mk_file(&pseudo, &best, file);
  if (chosen && ok) {
    *chosen = best;
    memset(&best, 0, sizeof(best));
  }
  candidate_free(&best);
  buf_free(&payload);
  return ok;
}

#ifndef QLIC_NO_MAIN
static int enc_anim_file(const Anim *anim, const wchar_t *out_path,
                         Candidate *chosen, uint64_t *written) {
  Buf file = {0};
  if (!enc_anim_mem(anim, &file, chosen)) {
    buf_free(&file);
    return 0;
  }
  int ok = wr_file(out_path, file.data, file.size);
  if (ok && written)
    *written = (uint64_t)file.size;
  buf_free(&file);
  return ok;
}

static int anim_ext_path(const wchar_t *path) {
  return has_ext(path, L".png") || has_ext(path, L".bmp") ||
         has_ext(path, L".tif") || has_ext(path, L".tiff") ||
         has_ext(path, L".ppm") || has_ext(path, L".gif");
}

static int anim_dir_for(const wchar_t *path, wchar_t *out, size_t cap) {
  if (wcslen(path) + 8 >= cap)
    return 0;
  wcscpy_s(out, cap, path);
  if (anim_ext_path(path)) {
    wchar_t *dot = wcsrchr(out, L'.');
    if (dot)
      *dot = 0;
    wcscat_s(out, cap, L"_frames");
  }
  return 1;
}

static int ensure_dir(const wchar_t *path) {
  DWORD a = GetFileAttributesW(path);
  if (a != INVALID_FILE_ATTRIBUTES) {
    if (a & FILE_ATTRIBUTE_DIRECTORY)
      return 1;
    set_err("animation output path is not a folder");
    return 0;
  }
  if (CreateDirectoryW(path, NULL))
    return 1;
  return fail_win32("CreateDirectory");
}

static int wr_anim_frames(const wchar_t *path, const Anim *anim,
                          wchar_t *written_dir, size_t cap) {
  wchar_t dir[32768];
  if (!anim_dir_for(path, dir, 32768)) {
    set_err("animation output path is too long");
    return 0;
  }
  if (!ensure_dir(dir))
    return 0;
  wchar_t sheet[32768];
  if (swprintf_s(sheet, 32768, L"%ls\\qlic-animation.txt", dir) < 0) {
    set_err("animation output path is too long");
    return 0;
  }
  FILE *meta = NULL;
  if (_wfopen_s(&meta, sheet, L"wb") != 0 || !meta) {
    set_err("could not write animation sheet");
    return 0;
  }
  fprintf(meta, "frames=%u\nloop=%u\n", anim->count, anim->loop_count);
  int ok = 1;
  for (uint32_t i = 0; i < anim->count; ++i) {
    wchar_t fp[32768];
    if (swprintf_s(fp, 32768, L"%ls\\frame_%06u.png", dir, i) < 0) {
      set_err("animation frame path is too long");
      ok = 0;
      break;
    }
    fprintf(meta, "frame_%06u.png delay_ms=%u width=%u height=%u\n", i,
            anim->frames[i].delay_ms, anim->frames[i].image.width,
            anim->frames[i].image.height);
    if (!wr_img(fp, &anim->frames[i].image)) {
      ok = 0;
      break;
    }
  }
  fclose(meta);
  if (ok && written_dir && cap)
    wcscpy_s(written_dir, cap, dir);
  return ok;
}

typedef struct {
  int enabled;
  int hdr_transfer;
  int alpha_explicit;
  uint32_t alpha_mode;
  uint32_t color_authority;
  uint16_t color_primaries;
  uint16_t transfer_characteristics;
  uint16_t matrix_coefficients;
  uint8_t full_range;
  const wchar_t *icc_path;
  const uint8_t *icc_data;
  size_t icc_size;
  const wchar_t *metadata_paths[QLIC_INPUT_MAX_METADATA + 4u];
  const uint8_t *metadata_data[QLIC_INPUT_MAX_METADATA + 4u];
  size_t metadata_sizes[QLIC_INPUT_MAX_METADATA + 4u];
  uint8_t metadata_tags[QLIC_INPUT_MAX_METADATA + 4u][4];
  uint32_t metadata_count;
  const char *name;
} CliColorDescription;

static int cli_color_description(int argc, wchar_t **argv,
                                 CliColorDescription *description) {
  memset(description, 0, sizeof(*description));
  description->alpha_mode = QSW2_ALPHA_STRAIGHT;
  const wchar_t *profile =
      arg_value(argc, argv, L"--color-profile", NULL, NULL);
  const wchar_t *icc = arg_value(argc, argv, L"--icc", NULL, NULL);
  const wchar_t *alpha = arg_value(argc, argv, L"--alpha", NULL, NULL);
  const wchar_t *metadata_paths[4] = {
      arg_value(argc, argv, L"--exif", NULL, NULL),
      arg_value(argc, argv, L"--xmp", NULL, NULL),
      arg_value(argc, argv, L"--iptc", NULL, NULL),
      arg_value(argc, argv, L"--jumbf", NULL, NULL)};
  static const uint8_t metadata_tags[4][4] = {
      {'E', 'X', 'I', 'F'}, {'X', 'M', 'P', '_'},
      {'I', 'P', 'T', 'C'}, {'J', 'U', 'M', 'B'}};
  if (profile && eq_arg(profile, L"none"))
    profile = NULL;
  if (profile && icc) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT,
                   "choose either --color-profile or --icc, not both");
    return 0;
  }
  if (icc) {
    description->enabled = 1;
    description->color_authority = QSW2_COLOR_ICC;
    description->icc_path = icc;
    description->name = "icc";
  } else if (profile) {
    description->enabled = 1;
    description->color_authority = QSW2_COLOR_CICP;
    description->matrix_coefficients = 0u;
    description->full_range = 1u;
    if (eq_arg(profile, L"srgb")) {
      description->color_primaries = 1u;
      description->transfer_characteristics = 13u;
      description->name = "sRGB";
    } else if (eq_arg(profile, L"display-p3")) {
      description->color_primaries = 12u;
      description->transfer_characteristics = 13u;
      description->name = "Display P3";
    } else if (eq_arg(profile, L"rec2100-pq")) {
      description->color_primaries = 9u;
      description->transfer_characteristics = 16u;
      description->hdr_transfer = 1;
      description->name = "Rec.2100 PQ";
    } else if (eq_arg(profile, L"rec2100-hlg")) {
      description->color_primaries = 9u;
      description->transfer_characteristics = 18u;
      description->hdr_transfer = 1;
      description->name = "Rec.2100 HLG";
    } else if (eq_arg(profile, L"rec2020-linear")) {
      description->color_primaries = 9u;
      description->transfer_characteristics = 8u;
      description->name = "linear Rec.2020";
    } else {
      set_err_status(QLIC_CORE_BAD_ARGUMENT,
                     "unknown color profile; use srgb, display-p3, rec2100-pq, "
                     "rec2100-hlg, or rec2020-linear");
      return 0;
    }
  }
  if (alpha) {
    description->enabled = 1;
    description->alpha_explicit = 1;
    if (eq_arg(alpha, L"straight"))
      description->alpha_mode = QSW2_ALPHA_STRAIGHT;
    else if (eq_arg(alpha, L"premultiplied"))
      description->alpha_mode = QSW2_ALPHA_PREMULTIPLIED;
    else {
      set_err_status(QLIC_CORE_BAD_ARGUMENT,
                     "--alpha must be straight or premultiplied");
      return 0;
    }
  }
  for (uint32_t index = 0; index < 4u; ++index) {
    if (metadata_paths[index]) {
      uint32_t destination = description->metadata_count++;
      description->metadata_paths[destination] = metadata_paths[index];
      memcpy(description->metadata_tags[destination], metadata_tags[index],
             4u);
      description->enabled = 1;
    }
  }
  if (description->enabled && !description->name)
    description->name = "photographic";
  return 1;
}

static int cli_attach_xmp_sidecar(const wchar_t *input,
                                  CliColorDescription *description,
                                  wchar_t path[32768]) {
  for (uint32_t index = 0; index < description->metadata_count; ++index) {
    if (memcmp(description->metadata_tags[index], "XMP_", 4u) == 0)
      return 1;
  }
  if (wcslen(input) + 5u >= 32768u || wcscpy_s(path, 32768, input))
    return 1;
  wchar_t *slash = wcsrchr(path, L'\\');
  wchar_t *forward = wcsrchr(path, L'/');
  if (forward && (!slash || forward > slash))
    slash = forward;
  wchar_t *dot = wcsrchr(path, L'.');
  if (!dot || (slash && dot < slash))
    dot = path + wcslen(path);
  if (wcscpy_s(dot, 32768u - (size_t)(dot - path), L".xmp"))
    return 1;
  DWORD attributes = GetFileAttributesW(path);
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY))
    return 1;
  if (description->metadata_count >= QLIC_INPUT_MAX_METADATA + 4u) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "too many photographic metadata blocks");
    return 0;
  }
  uint32_t index = description->metadata_count++;
  description->metadata_paths[index] = path;
  memcpy(description->metadata_tags[index], "XMP_", 4u);
  description->enabled = 1;
  if (!description->name)
    description->name = "photographic";
  return 1;
}

static int cli_attach_input_metadata(const QlicInput *input,
                                     CliColorDescription *description) {
  if (input->icc_size && description->color_authority == QSW2_COLOR_UNSPECIFIED) {
    description->icc_data = input->icc;
    description->icc_size = input->icc_size;
    description->color_authority = QSW2_COLOR_ICC;
    description->enabled = 1;
    description->name = "embedded ICC";
  }
  for (uint32_t source = 0; source < input->metadata_count; ++source) {
    const QlicInputMetadata *metadata = input->metadata + source;
    int typed = !memcmp(metadata->tag, "EXIF", 4u) ||
                !memcmp(metadata->tag, "XMP_", 4u) ||
                !memcmp(metadata->tag, "IPTC", 4u) ||
                !memcmp(metadata->tag, "JUMB", 4u);
    int duplicate = 0;
    if (typed) {
      for (uint32_t index = 0; index < description->metadata_count; ++index) {
        if (!memcmp(description->metadata_tags[index], metadata->tag, 4u)) {
          duplicate = 1;
          break;
        }
      }
    }
    if (duplicate)
      continue;
    if (description->metadata_count >= QLIC_INPUT_MAX_METADATA + 4u) {
      set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                     "too many source metadata blocks");
      return 0;
    }
    uint32_t destination = description->metadata_count++;
    memcpy(description->metadata_tags[destination], metadata->tag, 4u);
    description->metadata_data[destination] = metadata->data;
    description->metadata_sizes[destination] = metadata->size;
    description->enabled = 1;
    if (!description->name)
      description->name = "embedded photographic";
  }
  return 1;
}

static int encode_described_wide(const WideImage *wide,
                                 const CliColorDescription *description,
                                 Buf *file) {
  if (description->hdr_transfer && wide->bits_per_sample <= 8u) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT,
                   "PQ and HLG require a source above 8 bits per sample");
    return 0;
  }
  if (description->alpha_explicit && wide->channels != 4u) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT,
                   "--alpha requires a source with an alpha channel");
    return 0;
  }
  Buf icc = {0};
  Buf metadata_files[QLIC_INPUT_MAX_METADATA + 4u] = {0};
  HdrMetadataBlock metadata[QLIC_INPUT_MAX_METADATA + 4u] = {0};
  size_t metadata_bytes = 0;
  if (description->icc_path) {
    if (!rd_file(description->icc_path, &icc))
      return 0;
    if (!icc.size || icc.size > QLIC_DEFAULT_MAX_METADATA_BYTES) {
      buf_free(&icc);
      set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                     "ICC profile exceeds the metadata limit");
      return 0;
    }
    metadata_bytes = icc.size;
  } else if (description->icc_size) {
    metadata_bytes = description->icc_size;
  }
  int metadata_ok = 1;
  for (uint32_t index = 0; index < description->metadata_count; ++index) {
    if (description->metadata_paths[index] &&
        !rd_file(description->metadata_paths[index], metadata_files + index)) {
      metadata_ok = 0;
      break;
    }
    const uint8_t *data = description->metadata_paths[index]
                              ? metadata_files[index].data
                              : description->metadata_data[index];
    size_t size = description->metadata_paths[index]
                      ? metadata_files[index].size
                      : description->metadata_sizes[index];
    if ((!data && size) ||
        !addok(metadata_bytes, size, &metadata_bytes) ||
        metadata_bytes > QLIC_DEFAULT_MAX_METADATA_BYTES) {
      if (metadata_bytes > QLIC_DEFAULT_MAX_METADATA_BYTES)
        set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                       "photographic metadata exceeds the metadata limit");
      metadata_ok = 0;
      break;
    }
    memcpy(metadata[index].tag, description->metadata_tags[index], 4u);
    metadata[index].data = (uint8_t *)data;
    metadata[index].size = size;
  }
  if (!metadata_ok) {
    for (uint32_t index = 0; index < description->metadata_count; ++index)
      buf_free(metadata_files + index);
    buf_free(&icc);
    return 0;
  }
  HdrImage image;
  memset(&image, 0, sizeof(image));
  image.wide = *wide;
  image.sample_type = QSW2_SAMPLE_UINT;
  image.alpha_mode =
      wide->channels == 4u ? description->alpha_mode : QSW2_ALPHA_NONE;
  image.color_authority = description->color_authority;
  image.icc = icc.size ? icc.data : (uint8_t *)description->icc_data;
  image.icc_size = icc.size ? icc.size : description->icc_size;
  if (description->color_authority == QSW2_COLOR_CICP) {
    image.has_cicp = 1u;
    image.color_primaries = description->color_primaries;
    image.transfer_characteristics = description->transfer_characteristics;
    image.matrix_coefficients = description->matrix_coefficients;
    image.full_range = description->full_range;
  }
  image.metadata = metadata;
  image.metadata_count = description->metadata_count;
  int ok = enc_hdr_mem(&image, file);
  for (uint32_t index = 0; index < description->metadata_count; ++index)
    buf_free(metadata_files + index);
  buf_free(&icc);
  return ok;
}

static int image_as_wide(const Image *image, WideImage *wide) {
  memset(wide, 0, sizeof(*wide));
  int mode = scan_mode(image);
  uint32_t channels = mode == MODE_GRAY ? 1u : mode == MODE_RGB ? 3u : 4u;
  size_t pixels = 0, samples = 0, bytes = 0;
  if (!mulok((size_t)image->width, (size_t)image->height, &pixels) ||
      !mulok(pixels, (size_t)channels, &samples) ||
      !mulok(samples, sizeof(uint16_t), &bytes)) {
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED,
                   "self-describing image dimensions overflow");
    return 0;
  }
  uint16_t *output = (uint16_t *)xmalloc(bytes);
  if (!output)
    return 0;
  for (size_t pixel = 0; pixel < pixels; ++pixel) {
    const uint8_t *source = image->rgba + pixel * 4u;
    uint16_t *destination = output + pixel * channels;
    if (channels == 1u) {
      destination[0] = source[0];
    } else {
      destination[0] = source[0];
      destination[1] = source[1];
      destination[2] = source[2];
      if (channels == 4u)
        destination[3] = source[3];
    }
  }
  wide->width = image->width;
  wide->height = image->height;
  wide->channels = channels;
  wide->bits_per_sample = 8u;
  wide->pixels = output;
  wide->pixels_size = bytes;
  wide->stride = (size_t)image->width * channels * sizeof(uint16_t);
  return 1;
}

static int hdr_as_u8_image(const HdrImage *hdr, Image *image) {
  if (!hdr || !image || hdr->wide.bits_per_sample != 8u ||
      (hdr->wide.channels != 1u && hdr->wide.channels != 3u &&
       hdr->wide.channels != 4u)) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "8-bit HDR export has an unsupported sample layout");
    return 0;
  }
  size_t pixels = 0, bytes = 0;
  if (!mulok((size_t)hdr->wide.width, (size_t)hdr->wide.height, &pixels) ||
      !mulok(pixels, 4u, &bytes))
    return 0;
  image->rgba = (uint8_t *)xmalloc(bytes);
  if (!image->rgba)
    return 0;
  image->width = hdr->wide.width;
  image->height = hdr->wide.height;
  const uint16_t *source = (const uint16_t *)hdr->wide.pixels;
  for (size_t pixel = 0; pixel < pixels; ++pixel) {
    uint8_t *destination = image->rgba + pixel * 4u;
    const uint16_t *sample = source + pixel * hdr->wide.channels;
    if (hdr->wide.channels == 1u) {
      destination[0] = destination[1] = destination[2] = (uint8_t)sample[0];
      destination[3] = 255u;
    } else {
      destination[0] = (uint8_t)sample[0];
      destination[1] = (uint8_t)sample[1];
      destination[2] = (uint8_t)sample[2];
      destination[3] =
          hdr->wide.channels == 4u ? (uint8_t)sample[3] : 255u;
    }
  }
  return 1;
}

static int hdr_sidecar_path(const wchar_t *output, const wchar_t *suffix,
                            wchar_t path[32768]) {
  if (swprintf_s(path, 32768, L"%ls%ls", output, suffix) < 0) {
    set_err("HDR sidecar path is too long");
    return 0;
  }
  return 1;
}

static int wr_hdr_sidecars(const wchar_t *output, const HdrImage *image) {
  wchar_t path[32768];
  if (image->icc_size) {
    if (!hdr_sidecar_path(output, L".icc", path) ||
        !wr_file(path, image->icc, image->icc_size))
      return 0;
  }
  for (uint32_t index = 0; index < image->metadata_count; ++index) {
    const HdrMetadataBlock *block = image->metadata + index;
    const wchar_t *extension = NULL;
    if (memcmp(block->tag, "EXIF", 4u) == 0)
      extension = L".exif";
    else if (memcmp(block->tag, "XMP_", 4u) == 0)
      extension = L".xmp";
    else if (memcmp(block->tag, "IPTC", 4u) == 0)
      extension = L".iptc";
    else if (memcmp(block->tag, "JUMB", 4u) == 0)
      extension = L".jumbf";
    uint32_t duplicate = 0;
    for (uint32_t previous = 0; previous < index; ++previous) {
      if (memcmp(image->metadata[previous].tag, block->tag, 4u) == 0)
        ++duplicate;
    }
    int path_ok = 0;
    if (extension && !duplicate)
      path_ok = hdr_sidecar_path(output, extension, path);
    else if (extension)
      path_ok = swprintf_s(path, 32768, L"%ls%ls.%u", output, extension,
                           duplicate + 1u) >= 0;
    else
      path_ok = swprintf_s(path, 32768,
                           L"%ls.meta-%02x%02x%02x%02x.%u", output,
                           block->tag[0], block->tag[1], block->tag[2],
                           block->tag[3], duplicate + 1u) >= 0;
    if (!path_ok) {
      set_err("HDR metadata sidecar path is too long");
      return 0;
    }
    if (!wr_file(path, block->data, block->size))
      return 0;
  }
  if (!hdr_sidecar_path(output, L".qlic-hdr.txt", path))
    return 0;
  FILE *manifest = NULL;
  if (_wfopen_s(&manifest, path, L"wb") != 0 || !manifest) {
    set_err("could not write HDR descriptor sidecar");
    return 0;
  }
  fprintf(manifest,
          "qlic-hdr-descriptor=1\nwidth=%u\nheight=%u\nchannels=%u\n"
          "bits-per-sample=%u\nsample-type=uint\nalpha-association=%u\n"
          "color-authority=%u\nicc-bytes=%zu\nmetadata-blocks=%u\n",
          image->wide.width, image->wide.height, image->wide.channels,
          image->wide.bits_per_sample, image->alpha_mode,
          image->color_authority, image->icc_size, image->metadata_count);
  if (image->has_cicp) {
    fprintf(manifest,
            "cicp=%u,%u,%u,%u\n",
            (unsigned)image->color_primaries,
            (unsigned)image->transfer_characteristics,
            (unsigned)image->matrix_coefficients, (unsigned)image->full_range);
  }
  if (image->has_mastering_display) {
    fprintf(manifest,
            "mastering-display=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
            (unsigned)image->primary_x[0], (unsigned)image->primary_y[0],
            (unsigned)image->primary_x[1], (unsigned)image->primary_y[1],
            (unsigned)image->primary_x[2], (unsigned)image->primary_y[2],
            (unsigned)image->white_x, (unsigned)image->white_y,
            image->max_luminance, image->min_luminance);
  }
  if (image->has_content_light)
    fprintf(manifest, "content-light=%u,%u\n", (unsigned)image->max_cll,
            (unsigned)image->max_fall);
  int ok = fclose(manifest) == 0;
  if (!ok)
    set_err("could not finish HDR descriptor sidecar");
  return ok;
}

typedef struct {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  uint32_t value;
  const uint8_t *data;
  size_t data_size;
} CliTiffEntry;

static int cli_tiff_entry_compare(const void *left, const void *right) {
  const CliTiffEntry *a = (const CliTiffEntry *)left;
  const CliTiffEntry *b = (const CliTiffEntry *)right;
  return a->tag < b->tag ? -1 : a->tag > b->tag ? 1 : 0;
}

static uint16_t cli_tiff_u16(const uint8_t *data, int little) {
  return little ? (uint16_t)((uint16_t)data[0] | (uint16_t)data[1] << 8u)
                : (uint16_t)((uint16_t)data[0] << 8u | data[1]);
}

static uint32_t cli_tiff_u32(const uint8_t *data, int little) {
  return little ? (uint32_t)data[0] | (uint32_t)data[1] << 8u |
                      (uint32_t)data[2] << 16u | (uint32_t)data[3] << 24u
                : (uint32_t)data[0] << 24u | (uint32_t)data[1] << 16u |
                      (uint32_t)data[2] << 8u | data[3];
}

static const uint8_t *cli_exif_tiff(const uint8_t *data, size_t size,
                                    size_t *tiff_size) {
  static const uint8_t exif_prefix[] = {'E', 'x', 'i', 'f', 0u, 0u};
  if (size >= sizeof(exif_prefix) &&
      !memcmp(data, exif_prefix, sizeof(exif_prefix))) {
    data += sizeof(exif_prefix);
    size -= sizeof(exif_prefix);
  }
  if (size >= 12u) {
    uint32_t offset = ((uint32_t)data[0] << 24u) |
                      ((uint32_t)data[1] << 16u) |
                      ((uint32_t)data[2] << 8u) | data[3];
    if ((uint64_t)offset + 4u <= size &&
        (!memcmp(data + 4u + offset, "II*\0", 4u) ||
         !memcmp(data + 4u + offset, "MM\0*", 4u))) {
      data += 4u + offset;
      size -= 4u + offset;
    }
  }
  if (size < 8u || (memcmp(data, "II*\0", 4u) &&
                    memcmp(data, "MM\0*", 4u)))
    return NULL;
  *tiff_size = size;
  return data;
}

static uint16_t cli_exif_orientation(const uint8_t *data, size_t size) {
  size_t tiff_size = 0;
  data = cli_exif_tiff(data, size, &tiff_size);
  if (!data)
    return 1u;
  int little = data[0] == 'I';
  uint32_t offset = cli_tiff_u32(data + 4u, little);
  if (offset > tiff_size || tiff_size - offset < 2u)
    return 1u;
  uint16_t count = cli_tiff_u16(data + offset, little);
  if ((size_t)count > (tiff_size - offset - 2u) / 12u)
    return 1u;
  const uint8_t *entry = data + offset + 2u;
  for (uint16_t index = 0; index < count; ++index, entry += 12u) {
    if (cli_tiff_u16(entry, little) == 274u &&
        cli_tiff_u16(entry + 2u, little) == 3u &&
        cli_tiff_u32(entry + 4u, little) == 1u) {
      uint16_t orientation = cli_tiff_u16(entry + 8u, little);
      return orientation >= 1u && orientation <= 8u ? orientation : 1u;
    }
  }
  return 1u;
}

static int cli_tiff_add(CliTiffEntry *entries, size_t capacity, size_t *count,
                        uint16_t tag, uint16_t type, uint32_t values,
                        uint32_t value, const void *data, size_t data_size) {
  if (*count >= capacity)
    return 0;
  CliTiffEntry *entry = entries + (*count)++;
  entry->tag = tag;
  entry->type = type;
  entry->count = values;
  entry->value = value;
  entry->data = (const uint8_t *)data;
  entry->data_size = data_size;
  return 1;
}

static int wr_hdr_tiff(const wchar_t *path, const HdrImage *image) {
  const WideImage *wide = &image->wide;
  if ((wide->bits_per_sample != 8u && wide->bits_per_sample != 16u) ||
      (wide->channels != 1u && wide->channels != 3u && wide->channels != 4u)) {
    set_err_status(QLIC_CORE_UNSUPPORTED_FORMAT,
                   "TIFF export requires 8/16-bit gray, RGB, or RGBA");
    return 0;
  }
  size_t storage = sizeof(uint16_t);
  size_t source_row_bytes = 0, row_bytes = 0, prefix = 0, required = 0;
  size_t pixel_bytes = 0;
  if (!wide->width || !wide->height ||
      !mulok((size_t)wide->width, (size_t)wide->channels * storage,
             &source_row_bytes) ||
      !mulok((size_t)wide->width,
             (size_t)wide->channels * (wide->bits_per_sample / 8u),
             &row_bytes) ||
      wide->stride < source_row_bytes ||
      !mulok((size_t)(wide->height - 1u), wide->stride, &prefix) ||
      !addok(prefix, source_row_bytes, &required) ||
      wide->pixels_size < required ||
      !mulok(row_bytes, (size_t)wide->height, &pixel_bytes) ||
      pixel_bytes > UINT32_MAX) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid TIFF image layout");
    return 0;
  }

  size_t capacity = 20u + image->metadata_count;
  CliTiffEntry *entries =
      (CliTiffEntry *)calloc(capacity, sizeof(*entries));
  if (!entries)
    return 0;
  size_t count = 0;
  uint16_t bits[4] = {(uint16_t)wide->bits_per_sample,
                      (uint16_t)wide->bits_per_sample,
                      (uint16_t)wide->bits_per_sample,
                      (uint16_t)wide->bits_per_sample};
  uint32_t xres[2] = {96u, 1u};
  uint32_t yres[2] = {96u, 1u};
  uint16_t orientation = 1u;
  int has_xmp = 0, has_iptc = 0, has_jumb = 0, has_8bim = 0;
  for (uint32_t index = 0; index < image->metadata_count; ++index) {
    const HdrMetadataBlock *block = image->metadata + index;
    if (!memcmp(block->tag, "pHYs", 4u) && block->size == 9u &&
        block->data[8] == 1u) {
      uint32_t px = ((uint32_t)block->data[0] << 24u) |
                    ((uint32_t)block->data[1] << 16u) |
                    ((uint32_t)block->data[2] << 8u) | block->data[3];
      uint32_t py = ((uint32_t)block->data[4] << 24u) |
                    ((uint32_t)block->data[5] << 16u) |
                    ((uint32_t)block->data[6] << 8u) | block->data[7];
      if (px) {
        xres[0] = px <= UINT32_MAX / 254u ? px * 254u : px;
        xres[1] = px <= UINT32_MAX / 254u ? 10000u : 1u;
      }
      if (py) {
        yres[0] = py <= UINT32_MAX / 254u ? py * 254u : py;
        yres[1] = py <= UINT32_MAX / 254u ? 10000u : 1u;
      }
    } else if (!memcmp(block->tag, "EXIF", 4u)) {
      uint16_t candidate = cli_exif_orientation(block->data, block->size);
      if (candidate != 1u)
        orientation = candidate;
    }
  }

#define TIFF_ADD(tag, type, values, value, data, bytes)                       \
  do {                                                                         \
    if (!cli_tiff_add(entries, capacity, &count, tag, type, values, value,     \
                      data, bytes))                                             \
      goto fail;                                                               \
  } while (0)
  TIFF_ADD(256u, 4u, 1u, wide->width, NULL, 0u);
  TIFF_ADD(257u, 4u, 1u, wide->height, NULL, 0u);
  if (wide->channels == 1u)
    TIFF_ADD(258u, 3u, 1u, wide->bits_per_sample, NULL, 0u);
  else
    TIFF_ADD(258u, 3u, wide->channels, 0u, bits,
             wide->channels * sizeof(uint16_t));
  TIFF_ADD(259u, 3u, 1u, 1u, NULL, 0u);
  TIFF_ADD(262u, 3u, 1u, wide->channels == 1u ? 1u : 2u, NULL, 0u);
  TIFF_ADD(273u, 4u, 1u, 0u, NULL, 0u);
  TIFF_ADD(274u, 3u, 1u, orientation, NULL, 0u);
  TIFF_ADD(277u, 3u, 1u, wide->channels, NULL, 0u);
  TIFF_ADD(278u, 4u, 1u, wide->height, NULL, 0u);
  TIFF_ADD(279u, 4u, 1u, (uint32_t)pixel_bytes, NULL, 0u);
  TIFF_ADD(282u, 5u, 1u, 0u, xres, sizeof(xres));
  TIFF_ADD(283u, 5u, 1u, 0u, yres, sizeof(yres));
  TIFF_ADD(284u, 3u, 1u, 1u, NULL, 0u);
  TIFF_ADD(296u, 3u, 1u, 2u, NULL, 0u);
  TIFF_ADD(339u, 3u, 1u, 1u, NULL, 0u);
  if (wide->channels == 4u)
    TIFF_ADD(338u, 3u, 1u,
             image->alpha_mode == QSW2_ALPHA_PREMULTIPLIED ? 1u : 2u,
             NULL, 0u);
  if (image->icc_size)
    TIFF_ADD(34675u, 7u, (uint32_t)image->icc_size, 0u, image->icc,
             image->icc_size);
  for (uint32_t index = 0; index < image->metadata_count; ++index) {
    const HdrMetadataBlock *block = image->metadata + index;
    if (block->size > UINT32_MAX)
      goto fail;
    if (!memcmp(block->tag, "XMP_", 4u) && !has_xmp++)
      TIFF_ADD(700u, 1u, (uint32_t)block->size, 0u, block->data, block->size);
    else if (!memcmp(block->tag, "IPTC", 4u) && !has_iptc++)
      TIFF_ADD(33723u, 7u, (uint32_t)block->size, 0u, block->data,
               block->size);
    else if (!memcmp(block->tag, "8BIM", 4u) && !has_8bim++)
      TIFF_ADD(34377u, 1u, (uint32_t)block->size, 0u, block->data,
               block->size);
    else if (!memcmp(block->tag, "JUMB", 4u) && !has_jumb++)
      TIFF_ADD(52502u, 7u, (uint32_t)block->size, 0u, block->data,
               block->size);
  }
#undef TIFF_ADD

  qsort(entries, count, sizeof(*entries), cli_tiff_entry_compare);
  if (count > UINT16_MAX)
    goto fail;
  uint64_t cursor = 8u + 2u + count * 12u + 4u;
  for (size_t index = 0; index < count; ++index) {
    if (entries[index].data_size > 4u) {
      cursor = (cursor + 1u) & ~UINT64_C(1);
      if (cursor > UINT32_MAX || entries[index].data_size > UINT32_MAX - cursor)
        goto fail;
      entries[index].value = (uint32_t)cursor;
      cursor += entries[index].data_size;
    } else if (entries[index].data_size) {
      uint32_t inline_value = 0u;
      memcpy(&inline_value, entries[index].data, entries[index].data_size);
      entries[index].value = inline_value;
    }
  }
  cursor = (cursor + 1u) & ~UINT64_C(1);
  if (cursor > UINT32_MAX || pixel_bytes > UINT32_MAX - cursor)
    goto fail;
  uint32_t pixel_offset = (uint32_t)cursor;
  for (size_t index = 0; index < count; ++index) {
    if (entries[index].tag == 273u) {
      entries[index].value = pixel_offset;
      break;
    }
  }
  Buf output = {0};
  static const uint8_t header[8] = {'I', 'I', 42u, 0u, 8u, 0u, 0u, 0u};
  if (!buf_append(&output, header, sizeof(header)) ||
      !buf_u16le(&output, (uint16_t)count))
    goto output_fail;
  for (size_t index = 0; index < count; ++index) {
    if (!buf_u16le(&output, entries[index].tag) ||
        !buf_u16le(&output, entries[index].type) ||
        !buf_u32le(&output, entries[index].count) ||
        !buf_u32le(&output, entries[index].value))
      goto output_fail;
  }
  if (!buf_u32le(&output, 0u))
    goto output_fail;
  for (size_t index = 0; index < count; ++index) {
    if (entries[index].data_size <= 4u)
      continue;
    while (output.size & 1u) {
      if (!buf_u8(&output, 0u))
        goto output_fail;
    }
    if (output.size != entries[index].value ||
        !buf_append(&output, entries[index].data, entries[index].data_size))
      goto output_fail;
  }
  while (output.size < pixel_offset) {
    if (!buf_u8(&output, 0u))
      goto output_fail;
  }
  for (uint32_t y = 0; y < wide->height; ++y) {
    const uint8_t *row = (const uint8_t *)wide->pixels + (size_t)y * wide->stride;
    if (wide->bits_per_sample == 16u) {
      if (!buf_append(&output, row, row_bytes))
        goto output_fail;
    } else {
      const uint16_t *samples = (const uint16_t *)row;
      size_t sample_count = (size_t)wide->width * wide->channels;
      for (size_t sample = 0; sample < sample_count; ++sample) {
        if (!buf_u8(&output, (uint8_t)samples[sample]))
          goto output_fail;
      }
    }
  }
  free(entries);
  int ok = wr_file(path, output.data, output.size);
  buf_free(&output);
  return ok;

output_fail:
  buf_free(&output);
fail:
  free(entries);
  if (!g_err[0])
    set_err_status(QLIC_CORE_LIMIT_EXCEEDED, "TIFF output exceeds its limits");
  return 0;
}

static uint32_t cli_png_be32(const uint8_t *data) {
  return (uint32_t)data[0] << 24u | (uint32_t)data[1] << 16u |
         (uint32_t)data[2] << 8u | data[3];
}

static int cli_png_chunk(Buf *output, const uint8_t tag[4],
                         const uint8_t *data, size_t size) {
  if (size > UINT32_MAX)
    return 0;
  uint8_t length[4] = {(uint8_t)(size >> 24u), (uint8_t)(size >> 16u),
                       (uint8_t)(size >> 8u), (uint8_t)size};
  Buf crc_data = {0};
  int ok = buf_append(&crc_data, tag, 4u) &&
           buf_append(&crc_data, data, size);
  uint32_t crc = ok ? stream_crc32(crc_data.data, crc_data.size) : 0u;
  uint8_t checksum[4] = {(uint8_t)(crc >> 24u), (uint8_t)(crc >> 16u),
                         (uint8_t)(crc >> 8u), (uint8_t)crc};
  if (ok)
    ok = buf_append(output, length, sizeof(length)) &&
         buf_append(output, tag, 4u) && buf_append(output, data, size) &&
         buf_append(output, checksum, sizeof(checksum));
  buf_free(&crc_data);
  return ok;
}

static int cli_png_hdr_chunks(Buf *output, const HdrImage *image,
                              uint32_t *embedded) {
  static const uint8_t xmp_prefix[] = {
      'X', 'M', 'L', ':', 'c', 'o', 'm', '.', 'a', 'd', 'o', 'b', 'e', '.',
      'x', 'm', 'p', 0u, 0u, 0u, 0u, 0u};
  int exif = 0, xmp = 0, jumb = 0, phys = 0, iccp = 0;
  for (uint32_t index = 0; index < image->metadata_count; ++index) {
    const HdrMetadataBlock *block = image->metadata + index;
    const uint8_t *data = block->data;
    size_t size = block->size;
    const uint8_t *tag = NULL;
    Buf packet = {0};
    if (!memcmp(block->tag, "EXIF", 4u) && !exif) {
      size_t tiff_size = 0;
      data = cli_exif_tiff(data, size, &tiff_size);
      if (!data)
        continue;
      size = tiff_size;
      tag = (const uint8_t *)"eXIf";
      exif = 1;
    } else if (!memcmp(block->tag, "XMP_", 4u) && !xmp) {
      if (!buf_append(&packet, xmp_prefix, sizeof(xmp_prefix)) ||
          !buf_append(&packet, data, size)) {
        buf_free(&packet);
        return 0;
      }
      data = packet.data;
      size = packet.size;
      tag = (const uint8_t *)"iTXt";
      xmp = 1;
    } else if (!memcmp(block->tag, "JUMB", 4u) && !jumb) {
      tag = (const uint8_t *)"caBX";
      jumb = 1;
    } else if (!memcmp(block->tag, "pHYs", 4u) && !phys) {
      if (size != 9u)
        continue;
      tag = block->tag;
      phys = 1;
    } else if (!memcmp(block->tag, "iTXt", 4u)) {
      tag = block->tag;
    } else if (!memcmp(block->tag, "iCCP", 4u) && !image->icc_size &&
               !iccp) {
      tag = block->tag;
      iccp = 1;
    }
    if (tag) {
      int ok = cli_png_chunk(output, tag, data, size);
      buf_free(&packet);
      if (!ok)
        return 0;
      ++*embedded;
    }
  }
  return 1;
}

static int wr_hdr_png_metadata(const wchar_t *path, const HdrImage *image) {
  static const uint8_t signature[8] = {137u, 80u, 78u, 71u,
                                        13u,  10u, 26u, 10u};
  Buf source = {0};
  if (!rd_file(path, &source))
    return 0;
  if (source.size < sizeof(signature) ||
      memcmp(source.data, signature, sizeof(signature))) {
    buf_free(&source);
    set_err_status(QLIC_CORE_BAD_DATA,
                   "WIC produced an invalid PNG during metadata export");
    return 0;
  }
  Buf output = {0};
  int ok = buf_append(&output, signature, sizeof(signature));
  int inserted = 0;
  uint32_t embedded = 0;
  for (size_t offset = sizeof(signature); ok && offset + 12u <= source.size;) {
    uint32_t length = cli_png_be32(source.data + offset);
    if ((size_t)length > source.size - offset - 12u) {
      ok = 0;
      break;
    }
    const uint8_t *tag = source.data + offset + 4u;
    size_t chunk_size = (size_t)length + 12u;
    if (!inserted && (!memcmp(tag, "IDAT", 4u) ||
                      !memcmp(tag, "IEND", 4u))) {
      ok = cli_png_hdr_chunks(&output, image, &embedded);
      inserted = 1;
    }
    if (ok)
      ok = buf_append(&output, source.data + offset, chunk_size);
    offset += chunk_size;
    if (!memcmp(tag, "IEND", 4u)) {
      if (offset != source.size)
        ok = 0;
      break;
    }
  }
  buf_free(&source);
  if (!ok || !inserted) {
    buf_free(&output);
    set_err_status(QLIC_CORE_BAD_DATA,
                   "could not embed metadata in the PNG output");
    return 0;
  }
  if (!embedded) {
    buf_free(&output);
    return 1;
  }
  wchar_t temporary[32768];
  if (!hdr_sidecar_path(path, L".metadata.tmp", temporary) ||
      !wr_file(temporary, output.data, output.size)) {
    buf_free(&output);
    return 0;
  }
  buf_free(&output);
  if (!MoveFileExW(temporary, path,
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary);
    return fail_win32("replace metadata-bearing PNG");
  }
  return 1;
}

static int wr_hdr_img(const wchar_t *path, const HdrImage *hdr) {
  if (!hdr || !hdr->wide.pixels) {
    set_err_status(QLIC_CORE_BAD_ARGUMENT, "invalid HDR image output");
    return 0;
  }
  int premultiplied = hdr->alpha_mode == QSW2_ALPHA_PREMULTIPLIED;
  int tiff = has_ext(path, L".tif") || has_ext(path, L".tiff");
  if (premultiplied && (hdr->wide.channels != 4u || !tiff)) {
    set_err_status(
        QLIC_CORE_UNSUPPORTED_FORMAT,
        "premultiplied samples require a four-channel associated-alpha TIFF; "
        "PNG alpha is straight and would be mislabeled");
    return 0;
  }
  int ok = 0;
  if (tiff) {
    ok = wr_hdr_tiff(path, hdr);
  } else if (hdr->wide.bits_per_sample == 8u) {
    Image image = {0};
    ok = hdr_as_u8_image(hdr, &image) &&
         wr_img_profile_alpha(path, &image, hdr->icc, hdr->icc_size,
                              premultiplied);
    image_free(&image);
  } else if (hdr->wide.bits_per_sample == 16u) {
    ok = wr_wide_img_profile_alpha(path, &hdr->wide, hdr->icc, hdr->icc_size,
                                   premultiplied);
  } else {
    set_err_status(
        QLIC_CORE_UNSUPPORTED_FORMAT,
        "CLI image export supports exact 8-bit or 16-bit HDR samples; use "
        "qlic_decode_hdr for other integer precisions");
    return 0;
  }
  if (ok && has_ext(path, L".png"))
    ok = wr_hdr_png_metadata(path, hdr);
  return ok && wr_hdr_sidecars(path, hdr);
}

static void usage(void) {
  printf("qlic - Quick Lossless Image Codec\n\n"
         "Commands:\n"
         "  qlic pack   <input-image> <output.qlic>\n"
         "  qlic unpack <input.qlic>  <output.png|bmp|tiff|ppm>\n"
         "  qlic unpack <animated.qlic> <frames-folder>\n"
         "  qlic view   <input.qlic>  [--save output.png]\n"
         "  qlic info   <input.qlic> [--json]\n"
         "  qlic verify <input.qlic> [--json]\n"
         "  qlic version\n\n"
         "16-bit native-wide images unpack to PNG or TIFF. Self-describing "
         "8/16-bit images embed compatible metadata and also write exact "
         "archival sidecars. Premultiplied alpha requires TIFF.\n\n"
         "Options:\n"
         "  --threads N|all           worker limit, default 1\n"
         "  --color-profile PROFILE  attach sRGB, Display P3, Rec.2100 PQ,\n"
         "                           Rec.2100 HLG, or linear Rec.2020 CICP\n"
         "  --icc FILE               attach an ICC profile instead of CICP\n"
         "  --alpha MODE             straight or premultiplied association\n"
         "  --exif FILE              preserve an opaque EXIF block\n"
         "  --xmp FILE               preserve an opaque XMP packet\n"
         "  --iptc FILE              preserve an opaque IPTC block\n"
         "  --jumbf FILE             preserve an opaque JUMBF/C2PA block\n"
         "  --json           machine-readable info or verification\n");
}

static int cmd_pack(int argc, wchar_t **argv) {
  wchar_t *pos[2] = {0};
  if (pos_args(argc, argv, 2, pos, 2) < 2) {
    usage();
    return 2;
  }
  CliColorDescription color;
  if (!cli_color_description(argc, argv, &color))
    return 2;
  wchar_t automatic_xmp[32768];
  if (!cli_attach_xmp_sidecar(pos[0], &color, automatic_xmp))
    return 2;
  QlicInput input = {0};
  char input_error[256] = {0};
  if (!qlic_input_open(pos[0], g_default_decode_limits.max_file_bytes,
                       g_default_decode_limits.max_pixels, &input, input_error,
                       sizeof(input_error))) {
    set_err("%s", input_error[0] ? input_error : "unsupported input image");
    return 1;
  }
  if (input.lossy) {
    fprintf(stderr,
            "warning: This source is lossy. QLIC preserves its decoded pixels "
            "losslessly, so the QLIC file will likely be larger.\n");
  }
  if (input.decoder == QLIC_INPUT_WIC && !input.icc_size &&
      color.color_authority == QSW2_COLOR_UNSPECIFIED) {
    Buf embedded_icc = {0};
    if (!rd_wic_icc(pos[0], &embedded_icc)) {
      qlic_input_close(&input);
      return 1;
    }
    if (embedded_icc.size) {
      input.icc = embedded_icc.data;
      input.icc_size = embedded_icc.size;
      input.owned_icc = embedded_icc.data;
    }
  }
  if (!cli_attach_input_metadata(&input, &color)) {
    qlic_input_close(&input);
    return 1;
  }
  if (input.bits_per_sample > 8u) {
    uint32_t channels = input.channels;
    uint32_t bits_per_sample = input.bits_per_sample;
    WideImage wide = {0};
    if (!rd_wide_img(pos[0], channels, bits_per_sample, input.alpha_mode,
                     &wide)) {
      qlic_input_close(&input);
      return 1;
    }
    Buf file = {0};
    double t0 = now_s();
    int ok = color.enabled
                 ? encode_described_wide(&wide, &color, &file)
                 : enc_wide_mem(wide.pixels, wide.pixels_size, wide.width,
                                wide.height, wide.stride, wide.channels,
                                wide.bits_per_sample, &file);
    double t1 = now_s();
    if (ok)
      ok = wr_file(pos[1], file.data, file.size);
    if (ok) {
      uint64_t raw = (uint64_t)wide.width * wide.height * wide.channels * 2u;
      uint64_t src = fsize(pos[0]);
      printf("%ux%u, %u-channel %u-bit image -> %zu bytes QLIC in %.3f s\n",
             wide.width, wide.height, wide.channels, wide.bits_per_sample,
             file.size, t1 - t0);
      printf("mode=%s source=%s raw ratio %.2fx",
             mname(color.enabled ? MODE_HDR_WIDE : MODE_NATIVE_WIDE),
             color.enabled ? color.name : "wide-native",
             file.size ? (double)raw / (double)file.size : 0.0);
      if (src)
        printf(", source-container ratio %.2fx",
               file.size ? (double)src / (double)file.size : 0.0);
      printf("\n");
    }
    buf_free(&file);
    wide_image_free(&wide);
    qlic_input_close(&input);
    return ok ? 0 : 1;
  }
  Anim anim = {0};
  int is_anim = 0;
  Image im = {0};
  if (input.decoder == QLIC_INPUT_WIC) {
    if (!rd_anim(pos[0], &anim, &is_anim)) {
      qlic_input_close(&input);
      return 1;
    }
    if (is_anim) {
      if (color.enabled) {
        anim_free(&anim);
        qlic_input_close(&input);
        set_err_status(
            QLIC_CORE_UNSUPPORTED_FORMAT,
            "self-describing color metadata is not available for animation");
        return 1;
      }
      Candidate chosen;
      memset(&chosen, 0, sizeof(chosen));
      uint64_t out_size = 0;
      double t0 = now_s();
      int ok = enc_anim_file(&anim, pos[1], &chosen, &out_size);
      double t1 = now_s();
      if (ok) {
        printf("%ux%u animation, %u frames -> %llu bytes QLIC in %.3f s\n",
               anim.width, anim.height, anim.count,
               (unsigned long long)out_size, t1 - t0);
        printf("mode=%s codec=%s payload=%llu compressed=%zu\n",
               mname(chosen.mode), cname(chosen.codec),
               (unsigned long long)chosen.payload_size, chosen.compressed_size);
      }
      anim_free(&anim);
      candidate_free(&chosen);
      qlic_input_close(&input);
      return ok ? 0 : 1;
    }
    if (!rd_img(pos[0], &im)) {
      qlic_input_close(&input);
      return 1;
    }
  } else {
    QlicInputImage decoded = {0};
    if (!qlic_input_decode(&input, g_default_decode_limits.max_pixels, &decoded,
                           input_error, sizeof(input_error))) {
      qlic_input_close(&input);
      set_err("%s", input_error[0] ? input_error : "could not decode input");
      return 1;
    }
    im.rgba = decoded.rgba;
    im.width = decoded.width;
    im.height = decoded.height;
  }
  if (color.enabled) {
    WideImage described = {0};
    Buf file = {0};
    double t0 = now_s();
    int ok = image_as_wide(&im, &described) &&
             encode_described_wide(&described, &color, &file);
    double t1 = now_s();
    if (ok)
      ok = wr_file(pos[1], file.data, file.size);
    if (ok) {
      uint64_t raw = (uint64_t)im.width * im.height * described.channels;
      uint64_t src = fsize(pos[0]);
      printf("%ux%u, %u-channel 8-bit image with %s metadata -> "
             "%zu bytes QLIC in %.3f s\n",
             im.width, im.height, described.channels, color.name, file.size,
             t1 - t0);
      printf("mode=%s source=%s raw ratio %.2fx", mname(MODE_HDR_WIDE),
             color.name, file.size ? (double)raw / (double)file.size : 0.0);
      if (src)
        printf(", source-container ratio %.2fx",
               file.size ? (double)src / (double)file.size : 0.0);
      printf("\n");
    }
    buf_free(&file);
    wide_image_free(&described);
    image_free(&im);
    qlic_input_close(&input);
    return ok ? 0 : 1;
  }
  Candidate chosen;
  memset(&chosen, 0, sizeof(chosen));
  uint64_t out_size = 0;
  double t0 = now_s();
  int ok = enc_file(&im, pos[1], &chosen, &out_size);
  double t1 = now_s();
  if (!ok) {
    image_free(&im);
    candidate_free(&chosen);
    qlic_input_close(&input);
    return 1;
  }
  uint64_t raw = (uint64_t)im.width * (uint64_t)im.height * 4u;
  uint64_t src = fsize(pos[0]);
  printf("%ux%u image -> %llu bytes QLIC in %.3f s\n", im.width, im.height,
         (unsigned long long)out_size, t1 - t0);
  printf(
      "mode=%s transform=%s codec=%s source=%s payload=%llu compressed=%zu\n",
      mname(chosen.mode), tname(chosen.transform), cname(chosen.codec),
      "native", (unsigned long long)chosen.payload_size,
      chosen.compressed_size);
  printf("raw-rgba ratio %.2fx",
         out_size ? (double)raw / (double)out_size : 0.0);
  if (src)
    printf(", source-container ratio %.2fx",
           out_size ? (double)src / (double)out_size : 0.0);
  printf("\n");
  image_free(&im);
  candidate_free(&chosen);
  qlic_input_close(&input);
  return 0;
}

static int cmd_unpack(int argc, wchar_t **argv) {
  wchar_t *pos[2] = {0};
  if (pos_args(argc, argv, 2, pos, 2) < 2) {
    usage();
    return 2;
  }
  Buf file = {0};
  Image im = {0};
  QlicHeader h = {0};
  if (!rd_file(pos[0], &file))
    return 1;
  if (file.size > 12u && file.data[12] == MODE_ANIM) {
    Anim anim = {0};
    double t0 = now_s();
    int ok = dec_anim_qlic(file.data, file.size, &anim, &h);
    double t1 = now_s();
    wchar_t dir[32768] = {0};
    if (ok)
      ok = wr_anim_frames(pos[1], &anim, dir, 32768);
    if (ok) {
      wprintf(
          L"%ux%u animation restored as %u PNG frames in %ls, decode %.3f s\n",
          anim.width, anim.height, anim.count, dir, t1 - t0);
    }
    anim_free(&anim);
    buf_free(&file);
    return ok ? 0 : 1;
  }
  if (file.size > 12u && file.data[12] == MODE_HDR_WIDE) {
    HdrImage hdr = {0};
    double t0 = now_s();
    int ok = dec_hdr_qlic_limited(file.data, file.size, &hdr, &h,
                                  &g_default_decode_limits);
    double t1 = now_s();
    if (ok)
      ok = wr_hdr_img(pos[1], &hdr);
    if (ok) {
      printf("%ux%u, %u-channel %u-bit self-describing image restored "
             "with metadata sidecars in %.3f s\n",
             hdr.wide.width, hdr.wide.height, hdr.wide.channels,
             hdr.wide.bits_per_sample, t1 - t0);
    }
    hdr_image_free(&hdr);
    buf_free(&file);
    return ok ? 0 : 1;
  }
  if (file.size > 12u && file.data[12] == MODE_NATIVE_WIDE) {
    WideImage wide = {0};
    double t0 = now_s();
    int ok = dec_wide_qlic_limited(file.data, file.size, &wide, &h,
                                   &g_default_decode_limits);
    double t1 = now_s();
    if (ok)
      ok = wr_wide_img(pos[1], &wide);
    if (ok) {
      printf("%ux%u, %u-channel %u-bit image restored in %.3f s\n", wide.width,
             wide.height, wide.channels, wide.bits_per_sample, t1 - t0);
    }
    wide_image_free(&wide);
    buf_free(&file);
    return ok ? 0 : 1;
  }
  g_err[0] = 0;
  double t0 = now_s();
  int ok = dec_qlic(file.data, file.size, &im, &h);
  double t1 = now_s();
  if (!ok) {
    buf_free(&file);
    return 1;
  }
  ok = wr_img(pos[1], &im);
  if (ok) {
    printf("%ux%u image restored in %.3f s\n", im.width, im.height, t1 - t0);
  }
  image_free(&im);
  buf_free(&file);
  return ok ? 0 : 1;
}

static int cmd_view(int argc, wchar_t **argv) {
  wchar_t *pos[1] = {0};
  if (pos_args(argc, argv, 2, pos, 1) < 1) {
    usage();
    return 2;
  }
  const wchar_t *save = arg_value(argc, argv, L"--save", L"-s", NULL);
  if (!save)
    save = arg_value(argc, argv, L"--out", L"-o", NULL);
  wchar_t *path = (wchar_t *)xmalloc(32768u * sizeof(wchar_t));
  if (!path)
    return 1;
  int tmp = save == NULL;
  if (save) {
    wcscpy_s(path, 32768u, save);
  } else if (!tmp_ext(L".png", path, 32768u)) {
    set_err("could not create temporary preview path");
    free(path);
    return 1;
  }

  Buf file = {0};
  Image im = {0};
  Anim anim = {0};
  QlicHeader h;
  if (!rd_file(pos[0], &file)) {
    free(path);
    return 1;
  }
  double t0 = now_s();
  int ok = dec_any_qlic(file.data, file.size, &anim, &h);
  double t1 = now_s();
  uint32_t frames = ok ? anim.count : 0u;
  if (ok) {
    im = anim.frames[0].image;
    memset(&anim.frames[0].image, 0, sizeof(anim.frames[0].image));
  }
  if (ok)
    ok = wr_img(path, &im);
  if (!ok) {
    if (tmp)
      DeleteFileW(path);
    image_free(&im);
    anim_free(&anim);
    buf_free(&file);
    free(path);
    return 1;
  }
  HINSTANCE sh = ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
  if ((INT_PTR)sh <= 32) {
    wprintf(L"%ux%u decoded preview written to %ls in %.3f s\n", im.width,
            im.height, path, t1 - t0);
    set_err("could not open the default image viewer");
    image_free(&im);
    anim_free(&anim);
    buf_free(&file);
    free(path);
    return 1;
  }
  wprintf(L"%ux%u QLIC preview opened from %ls in %.3f s\n", im.width,
          im.height, path, t1 - t0);
  if (frames > 1u)
    printf("previewed the first of %u animation frames\n", frames);
  image_free(&im);
  anim_free(&anim);
  buf_free(&file);
  free(path);
  return 0;
}

static void info_blocks(const uint8_t *p, size_t n, uint32_t w, uint32_t h) {
  if (n < 4)
    return;
  if (n >= 20 && memcmp(p, "QCF1", 4) == 0) {
    int ch = p[5];
    if (p[4] != CF_SIZE || (ch != 1 && ch != 3 && ch != 4))
      return;
    size_t table = (size_t)rd32(p + 8);
    uint64_t z = rd64(p + 12);
    uint64_t rx = (w + CF_SIZE - 1u) / CF_SIZE;
    uint64_t ry = (h + CF_SIZE - 1u) / CF_SIZE;
    uint64_t regions = rx * ry;
    printf("block-stream=coordinate-field region=%u channels=%d regions=%llu "
           "table=%zu residual=%llu\n",
           CF_SIZE, ch, (unsigned long long)regions, table,
           (unsigned long long)z);
    if (regions && table == regions * 3u) {
      uint64_t q[5] = {0, 0, 0, 0, 0};
      for (uint64_t i = 0; i < regions; ++i) {
        const uint8_t *par = p + 20u + (size_t)i * 3u;
        q[0] += pdm_q(par, 0);
        q[1] += pdm_q(par, 1);
        q[2] += pdm_q(par, 2);
        q[3] += pdm_q(par, 3);
        q[4] += pdm_q(par, 4);
      }
      printf("coordinates avg=%llu,%llu,%llu,%llu,%llu scale=0..15\n",
             (unsigned long long)(q[0] / regions),
             (unsigned long long)(q[1] / regions),
             (unsigned long long)(q[2] / regions),
             (unsigned long long)(q[3] / regions),
             (unsigned long long)(q[4] / regions));
    }
    return;
  }
  if (n >= 20 && memcmp(p, "QPD1", 4) == 0) {
    int ch = p[5];
    if (p[4] != PDM_SIZE || (ch != 1 && ch != 3 && ch != 4))
      return;
    size_t table = (size_t)rd32(p + 8);
    uint64_t z = rd64(p + 12);
    uint64_t rx = (w + PDM_SIZE - 1u) / PDM_SIZE;
    uint64_t ry = (h + PDM_SIZE - 1u) / PDM_SIZE;
    uint64_t regions = rx * ry;
    size_t parn = blk2_parn(BLK2_PDM, ch);
    printf("block-stream=pdm-region region=%u channels=%d regions=%llu "
           "table=%zu residual=%llu\n",
           PDM_SIZE, ch, (unsigned long long)regions, table,
           (unsigned long long)z);
    if (regions && table >= regions * parn) {
      uint64_t q[5] = {0, 0, 0, 0, 0};
      for (uint64_t i = 0; i < regions; ++i) {
        const uint8_t *par = p + 20u + (size_t)i * parn;
        q[0] += pdm_q(par, 0);
        q[1] += pdm_q(par, 1);
        q[2] += pdm_q(par, 2);
        q[3] += pdm_q(par, 3);
        q[4] += pdm_q(par, 4);
      }
      printf("pdm-coordinates avg=%llu,%llu,%llu,%llu,%llu scale=0..15\n",
             (unsigned long long)(q[0] / regions),
             (unsigned long long)(q[1] / regions),
             (unsigned long long)(q[2] / regions),
             (unsigned long long)(q[3] / regions),
             (unsigned long long)(q[4] / regions));
    }
    return;
  }
  if (n >= 20 && memcmp(p, "QBL2", 4) == 0) {
    int ch = p[5];
    if (p[4] != BLK_SIZE || (ch != 1 && ch != 3 && ch != 4))
      return;
    size_t table = (size_t)rd32(p + 8);
    uint64_t z = rd64(p + 12);
    uint64_t bx = (w + BLK_SIZE - 1u) / BLK_SIZE;
    uint64_t by = (h + BLK_SIZE - 1u) / BLK_SIZE;
    uint64_t blocks = bx * by;
    uint64_t count[7] = {0, 0, 0, 0, 0, 0, 0};
    size_t pos = 20u;
    for (uint64_t i = 0; i < blocks && pos < 20u + table; ++i) {
      uint8_t op = p[pos++];
      if (op > BLK2_PDM)
        break;
      ++count[op];
      size_t parn = blk2_parn(op, ch);
      if (parn > 20u + table - pos)
        break;
      pos += parn;
    }
    printf("block-stream=block-residual block=%u channels=%d blocks=%llu "
           "table=%zu residual=%llu\n",
           BLK_SIZE, ch, (unsigned long long)blocks, table,
           (unsigned long long)z);
    printf("ops zero=%llu flat=%llu grad=%llu left=%llu up=%llu causal=%llu "
           "pdm=%llu\n",
           (unsigned long long)count[0], (unsigned long long)count[1],
           (unsigned long long)count[2], (unsigned long long)count[3],
           (unsigned long long)count[4], (unsigned long long)count[5],
           (unsigned long long)count[6]);
    return;
  }
  if (n >= 8 && (memcmp(p, "QBL1", 4) == 0 || memcmp(p, "QBR1", 4) == 0))
    printf("block-stream=%s block=%u channels=%d\n",
           memcmp(p, "QBR1", 4) == 0 ? "block-reference" : "block-palette",
           BLK_SIZE, p[5]);
}

static int cmd_info(int argc, wchar_t **argv) {
  wchar_t *pos[1] = {0};
  if (pos_args(argc, argv, 2, pos, 1) < 1) {
    usage();
    return 2;
  }
  Buf file = {0};
  Buf payload = {0};
  QlicHeader h = {0};
  StreamInfo native_info = {0};
  int has_native_info = 0;
  HdrInfo hdr_info = {0};
  int has_hdr_info = 0;
  int json = has_option(argc, argv, L"--json");
  if (!rd_file(pos[0], &file))
    return 1;
  int ok = rd_head(file.data, file.size, &h);
  if (ok && h.mode == MODE_HDR_WIDE) {
    ok = hdr_qlic_info_limited(file.data, file.size, &hdr_info, NULL, NULL);
    has_hdr_info = ok;
  } else if (ok && (h.mode == MODE_NATIVE || h.mode == MODE_BLOCKS)) {
    size_t palette_size = file_palette_size(&h);
    const uint8_t *compressed = file.data + QLIC_HEADER_SIZE + palette_size;
    if (!unzip(compressed, (size_t)h.compressed_size, h.codec, h.payload_size,
               &payload, NULL)) {
      ok = 0;
    } else if (h.mode == MODE_NATIVE) {
      /* predictor details live in QST1 rather than the outer container */
      int err = stream_get_info(payload.data, payload.size, &native_info);
      if (err != STREAM_OK) {
        set_err_status(stream_failure_status(err, 1),
                       "native stream metadata failed: %s",
                       stream_strerror(err));
        ok = 0;
      } else if (native_info.width != h.width ||
                 native_info.height != h.height) {
        set_err("corrupt file: native stream dimensions mismatch");
        ok = 0;
      } else {
        has_native_info = 1;
      }
    }
  }
  if (ok) {
    if (json) {
      printf("{\"width\":%u,\"height\":%u,\"mode\":%d,"
             "\"mode_name\":\"%s\",\"transform\":%d,"
             "\"transform_name\":\"%s\",\"codec\":%d,"
             "\"codec_name\":\"%s\",\"payload_bytes\":%llu,"
             "\"compressed_bytes\":%llu,\"file_bytes\":%zu,"
             "\"frame_count\":%u,\"animated\":%s",
             h.width, h.height, h.mode, mname(h.mode), h.transform,
             tname(h.transform), h.codec, cname(h.codec),
             (unsigned long long)h.payload_size,
             (unsigned long long)h.compressed_size, file.size,
             h.mode == MODE_ANIM ? h.palette_count : 1u,
             h.mode == MODE_ANIM ? "true" : "false");
      if (has_native_info) {
        printf(",\"native\":{\"mode\":%d,\"transform\":%d,"
               "\"tile_log\":%d,\"adaptation\":%d,\"channels\":%d,"
               "\"sample_bits\":%d}",
               native_info.mode, native_info.transform, native_info.tile_log,
               native_info.adaptation, native_info.channels,
               native_info.sample_bits);
      }
      if (h.mode == MODE_NATIVE_WIDE) {
        printf(",\"wide\":{\"channels\":%u,\"bits_per_sample\":%d}",
               h.palette_count, h.index_bits);
      } else if (h.mode == MODE_HDR_WIDE && has_hdr_info) {
        printf(",\"hdr\":{\"channels\":%u,\"bits_per_sample\":%u,"
               "\"sample_type\":\"uint\",\"alpha\":%u,"
               "\"color_authority\":%u,\"icc\":%s,\"cicp\":%s,"
               "\"mastering_display\":%s,\"content_light\":%s,"
               "\"metadata_blocks\":%u}",
               hdr_info.channels, hdr_info.bits_per_sample, hdr_info.alpha_mode,
               hdr_info.color_authority, hdr_info.has_icc ? "true" : "false",
               hdr_info.has_cicp ? "true" : "false",
               hdr_info.has_mastering_display ? "true" : "false",
               hdr_info.has_content_light ? "true" : "false",
               hdr_info.metadata_count);
      }
      printf("}\n");
    } else {
      printf("QLIC %ux%u mode=%s transform=%s codec=%s\n", h.width, h.height,
             mname(h.mode), tname(h.transform), cname(h.codec));
      if (has_native_info) {
        printf(
            "native-mode=%d native-transform=%d native-tile-log=%d "
            "native-adaptation=%d native-channels=%d native-sample-bits=%d\n",
            native_info.mode, native_info.transform, native_info.tile_log,
            native_info.adaptation, native_info.channels,
            native_info.sample_bits);
      }
      if (h.mode == MODE_ANIM) {
        printf("frames=%u payload=%llu compressed=%llu total=%zu\n",
               h.palette_count, (unsigned long long)h.payload_size,
               (unsigned long long)h.compressed_size, file.size);
      } else if (h.mode == MODE_NATIVE_WIDE) {
        printf("channels=%u bits-per-sample=%d payload=%llu total=%zu\n",
               h.palette_count, h.index_bits,
               (unsigned long long)h.payload_size, file.size);
      } else if (h.mode == MODE_HDR_WIDE && has_hdr_info) {
        printf("channels=%u bits-per-sample=%u sample-type=uint alpha=%u "
               "color-authority=%u icc=%u cicp=%u mastering-display=%u "
               "content-light=%u metadata-blocks=%u payload=%llu total=%zu\n",
               hdr_info.channels, hdr_info.bits_per_sample, hdr_info.alpha_mode,
               hdr_info.color_authority, hdr_info.has_icc, hdr_info.has_cicp,
               hdr_info.has_mastering_display, hdr_info.has_content_light,
               hdr_info.metadata_count,
               (unsigned long long)h.payload_size, file.size);
      } else if (h.mode == MODE_TILES || h.mode == MODE_TILE_MODEL) {
        printf("tile-height=%u channels=%d payload=%llu compressed=%llu "
               "total=%zu\n",
               h.palette_count, h.index_bits,
               (unsigned long long)h.payload_size,
               (unsigned long long)h.compressed_size, file.size);
      } else {
        printf(
            "palette=%u index-bits=%d payload=%llu compressed=%llu total=%zu\n",
            h.palette_count, h.index_bits, (unsigned long long)h.payload_size,
            (unsigned long long)h.compressed_size, file.size);
      }
      if (h.mode == MODE_BLOCKS)
        info_blocks(payload.data, payload.size, h.width, h.height);
    }
  }
  if (h.mode == MODE_NATIVE || h.mode == MODE_BLOCKS)
    unzip_free(&payload, h.codec);
  buf_free(&file);
  return ok ? 0 : 1;
}

static void print_json_text(const char *text) {
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)text; p && *p; ++p) {
    if (*p == '"' || *p == '\\') {
      putchar('\\');
      putchar(*p);
    } else if (*p == '\n') {
      fputs("\\n", stdout);
    } else if (*p == '\r') {
      fputs("\\r", stdout);
    } else if (*p == '\t') {
      fputs("\\t", stdout);
    } else if (*p < 0x20u) {
      printf("\\u%04x", (unsigned)*p);
    } else {
      putchar(*p);
    }
  }
  putchar('"');
}

static int cmd_verify(int argc, wchar_t **argv) {
  wchar_t *pos[1] = {0};
  if (pos_args(argc, argv, 2, pos, 1) < 1) {
    usage();
    return 2;
  }
  int json = has_option(argc, argv, L"--json");
  Buf file = {0};
  QlicHeader header = {0};
  uint32_t frames_verified = 0;
  uint32_t channels = 0;
  uint32_t bits_per_sample = 0;
  int ok = rd_file(pos[0], &file) && rd_head(file.data, file.size, &header);
  if (ok && header.mode == MODE_NATIVE_WIDE) {
    WideImage image = {0};
    ok = dec_wide_qlic_limited(file.data, file.size, &image, NULL, NULL);
    if (ok) {
      frames_verified = 1u;
      channels = image.channels;
      bits_per_sample = image.bits_per_sample;
    }
    wide_image_free(&image);
  } else if (ok && header.mode == MODE_HDR_WIDE) {
    HdrImage image = {0};
    ok = dec_hdr_qlic_limited(file.data, file.size, &image, NULL, NULL);
    if (ok) {
      frames_verified = 1u;
      channels = image.wide.channels;
      bits_per_sample = image.wide.bits_per_sample;
    }
    hdr_image_free(&image);
  } else if (ok) {
    Anim animation = {0};
    ok = dec_any_qlic_limited(file.data, file.size, &animation, NULL, NULL);
    if (ok) {
      frames_verified = animation.count;
      channels = 4u;
      bits_per_sample = 8u;
    }
    anim_free(&animation);
  }

  if (json) {
    if (ok) {
      printf("{\"valid\":true,\"width\":%u,\"height\":%u,"
             "\"mode\":%d,\"frames_verified\":%u,\"channels\":%u,"
             "\"bits_per_sample\":%u}\n",
             header.width, header.height, header.mode, frames_verified,
             channels, bits_per_sample);
    } else {
      fputs("{\"valid\":false,\"error\":", stdout);
      print_json_text(g_err[0] ? g_err : "verification failed");
      fputs("}\n", stdout);
    }
  } else if (ok) {
    printf("valid QLIC %ux%u mode=%s frames=%u channels=%u bits=%u\n",
           header.width, header.height, mname(header.mode), frames_verified,
           channels, bits_per_sample);
  }
  buf_free(&file);
  return ok ? 0 : 1;
}

static int cmd_version(int argc, wchar_t **argv) {
  (void)argc;
  (void)argv;
  printf("QLIC %s\n", QLIC_VERSION);
  printf("native stream: QST1 adaptive-range\n");
  printf("container: QLIC\n");
  return 0;
}

int wmain(_In_ int argc, _In_reads_(argc) wchar_t **argv) {
  HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(co)) {
    fprintf(stderr, "error: COM init failed: 0x%08lx\n", (unsigned long)co);
    return 1;
  }
  if (argc < 1 || !argv) {
    CoUninitialize();
    return 2;
  }

  runtime_init();
  int rc = 0;
  int kind = 0;
  wchar_t *cmd = argp(argc, argv, 1);
  if (!cmd || eq_arg(cmd, L"--help") || eq_arg(cmd, L"-h")) {
    usage();
    rc = 0;
  } else {
    kind = cli_kind(cmd);
    /* one thread keeps runs comparable, larger values are always explicit */
    unsigned threads = 1u;
    int has_threads = 0;
    if (!kind) {
      set_err_status(QLIC_CORE_BAD_ARGUMENT, "unknown command");
      usage();
      rc = 2;
    } else if (!cli_validate(argc, argv, kind, &threads, &has_threads)) {
      rc = 2;
    } else {
      if (has_threads)
        qlic_core_set_thread_count(threads);
    }
  }
  if (!rc && kind == CLI_PACK) {
    rc = cmd_pack(argc, argv);
  } else if (!rc && kind == CLI_UNPACK) {
    rc = cmd_unpack(argc, argv);
  } else if (!rc && kind == CLI_VIEW) {
    rc = cmd_view(argc, argv);
  } else if (!rc && kind == CLI_INFO) {
    rc = cmd_info(argc, argv);
  } else if (!rc && kind == CLI_VERIFY) {
    rc = cmd_verify(argc, argv);
  } else if (!rc && kind == CLI_VERSION) {
    rc = cmd_version(argc, argv);
  }

  if (rc != 0 && g_err[0]) {
    fprintf(stderr, "error: %s\n", g_err);
  }
  CoUninitialize();
  return rc;
}
#endif
