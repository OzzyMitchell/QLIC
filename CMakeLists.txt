cmake_minimum_required(VERSION 3.25)

project(qlic VERSION 1.0.1 LANGUAGES C)

if(WIN32)
  enable_language(RC)
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

function(qlic_windows_target target)
  target_compile_definitions(${target} PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS ${ARGN})
endfunction()

function(qlic_windows_unicode_target target)
  qlic_windows_target(${target} UNICODE _UNICODE ${ARGN})
endfunction()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(QLIC_SANITIZE "Build QLIC with compiler sanitizers" OFF)
option(QLIC_LIBFUZZER "Build the Clang coverage-guided decoder fuzzer" OFF)
option(QLIC_BUILD_CLI "Build the QLIC command line tool" ON)
option(QLIC_BUILD_EXAMPLES "Build QLIC SDK examples" ON)
option(QLIC_BUILD_WINDOWS_APPS "Build the Windows GUI, WIC decoder, and browser app" ON)
set(QLIC_LINUX_LZMS_DEFAULT OFF)
if(UNIX AND NOT APPLE)
  set(QLIC_LINUX_LZMS_DEFAULT ON)
endif()
option(QLIC_LINUX_LZMS "Use wimlib for LZMS encoding on Linux"
  ${QLIC_LINUX_LZMS_DEFAULT})
option(QLIC_STREAM_TRACE "Build native stream candidate timing diagnostics" OFF)
option(QLIC_BENCHMARK_TRIAL "Build the internal forced-mode benchmark" OFF)
if(QLIC_STREAM_TRACE)
  add_compile_definitions(QLIC_STREAM_TRACE)
endif()
set(QLIC_PACKAGE_NEEDS_WIMLIB FALSE)
if(UNIX AND NOT APPLE AND QLIC_LINUX_LZMS)
  find_path(QLIC_WIMLIB_INCLUDE_DIR NAMES wimlib.h REQUIRED)
  find_library(QLIC_WIMLIB_LIBRARY NAMES wim REQUIRED)
  if(QLIC_WIMLIB_LIBRARY MATCHES "\\.a$")
    message(FATAL_ERROR "QLIC requires the shared wimlib library")
  endif()
  add_library(qlic_wimlib SHARED IMPORTED)
  set_target_properties(qlic_wimlib PROPERTIES
    IMPORTED_LOCATION "${QLIC_WIMLIB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${QLIC_WIMLIB_INCLUDE_DIR}")
  set(QLIC_PACKAGE_NEEDS_WIMLIB TRUE)
endif()
set(QLIC_PGO "OFF" CACHE STRING "Profile-guided optimization mode")
set_property(CACHE QLIC_PGO PROPERTY STRINGS OFF GENERATE USE)
set(QLIC_PGO_PROFILE "" CACHE FILEPATH "Merged LLVM profile for QLIC_PGO=USE")
option(QLIC_NATIVE "Optimize for the build machine CPU" OFF)
string(TOUPPER "${QLIC_PGO}" QLIC_PGO)
if(NOT QLIC_PGO MATCHES "^(OFF|GENERATE|USE)$")
  message(FATAL_ERROR "QLIC_PGO must be OFF, GENERATE, or USE")
endif()
if(NOT QLIC_PGO STREQUAL "OFF" AND
   NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "QLIC_PGO requires Clang")
endif()
if(QLIC_PGO STREQUAL "USE" AND NOT EXISTS "${QLIC_PGO_PROFILE}")
  message(FATAL_ERROR "QLIC_PGO_PROFILE must name a merged LLVM profile")
endif()
if(QLIC_SANITIZE AND NOT QLIC_PGO STREQUAL "OFF")
  message(FATAL_ERROR "QLIC_SANITIZE and QLIC_PGO cannot be combined")
endif()
if(QLIC_LIBFUZZER AND (NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR MSVC))
  message(FATAL_ERROR "QLIC_LIBFUZZER requires Clang on a non-MSVC platform")
endif()
if(QLIC_NATIVE AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "QLIC_NATIVE requires Clang or GCC")
endif()

set(QLIC_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${QLIC_GENERATED_DIR}")
configure_file(cmake/qlic_version.h.in "${QLIC_GENERATED_DIR}/qlic_version.h" @ONLY)
set(QLIC_CLI_RES)
set(QLIC_GUI_RES)
set(QLIC_LIBRARY_RES)
set(QLIC_WIC_RES)
if(WIN32)
  set(QLIC_FILE_DESCRIPTION "Quick Lossless Image Codec")
  set(QLIC_INTERNAL_NAME "qlic")
  set(QLIC_ORIGINAL_FILENAME "qlic.exe")
  configure_file(codec/src/qlic.rc.in "${QLIC_GENERATED_DIR}/qlic-cli.rc" @ONLY)
  set(QLIC_CLI_RES "${QLIC_GENERATED_DIR}/qlic-cli.rc")

  set(QLIC_FILE_DESCRIPTION "QLIC Image Compressor")
  set(QLIC_INTERNAL_NAME "qlic-gui")
  set(QLIC_ORIGINAL_FILENAME "qlic-gui.exe")
  configure_file(codec/src/qlic.rc.in "${QLIC_GENERATED_DIR}/qlic-gui.rc" @ONLY)
  set(QLIC_GUI_RES "${QLIC_GENERATED_DIR}/qlic-gui.rc")

  set(QLIC_FILE_DESCRIPTION "QLIC SDK")
  set(QLIC_INTERNAL_NAME "qlic")
  set(QLIC_ORIGINAL_FILENAME "qlic.dll")
  configure_file(codec/src/qlic.rc.in "${QLIC_GENERATED_DIR}/qlic-library.rc" @ONLY)
  set(QLIC_LIBRARY_RES "${QLIC_GENERATED_DIR}/qlic-library.rc")

  set(QLIC_FILE_DESCRIPTION "QLIC WIC Decoder")
  set(QLIC_INTERNAL_NAME "qlic-wic")
  set(QLIC_ORIGINAL_FILENAME "qlic-wic.dll")
  configure_file(codec/src/qlic.rc.in "${QLIC_GENERATED_DIR}/qlic-wic.rc" @ONLY)
  set(QLIC_WIC_RES "${QLIC_GENERATED_DIR}/qlic-wic.rc")
endif()
set(QLIC_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/qlic")
configure_package_config_file(
  cmake/qlicConfig.cmake.in
  "${QLIC_GENERATED_DIR}/qlicConfig.cmake"
  INSTALL_DESTINATION "${QLIC_CMAKE_INSTALL_DIR}")
write_basic_package_version_file(
  "${QLIC_GENERATED_DIR}/qlicConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMinorVersion)
string(REPLACE "\\" "/" QLIC_PC_LIBDIR_NORMALIZED "${CMAKE_INSTALL_LIBDIR}")
string(REPLACE "/" ";" QLIC_PC_LIBDIR_PARTS "${QLIC_PC_LIBDIR_NORMALIZED}")
set(QLIC_PC_PREFIX_RELATIVE "..")
foreach(QLIC_PC_LIBDIR_PART IN LISTS QLIC_PC_LIBDIR_PARTS)
  if(NOT QLIC_PC_LIBDIR_PART STREQUAL "")
    string(APPEND QLIC_PC_PREFIX_RELATIVE "/..")
  endif()
endforeach()
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  set(QLIC_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
  set(QLIC_PC_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
  set(QLIC_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
  set(QLIC_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()
configure_file(cmake/qlic.pc.in "${QLIC_GENERATED_DIR}/qlic.pc" @ONLY)
include_directories("${QLIC_GENERATED_DIR}")

set(QLIC_MAP_SOURCES)
if(MSVC AND CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
  list(APPEND QLIC_MAP_SOURCES codec/src/map_avx2.c)
  set_source_files_properties(codec/src/map_avx2.c PROPERTIES COMPILE_OPTIONS /arch:AVX2)
endif()
set(QLIC_BUNDLED_IMAGE_CODECS FALSE)
if(WIN32 AND CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
  set(QLIC_BUNDLED_IMAGE_CODECS TRUE)
endif()

# sharing the object target keeps static and DLL codec builds identical
set(QLIC_CORE_SOURCES
  codec/src/qlic.c
  codec/src/stream.c
  codec/src/parallel.c
  codec/src/lzms.c
  ${QLIC_MAP_SOURCES})
add_library(qliccore OBJECT ${QLIC_CORE_SOURCES})
set_target_properties(qliccore PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)
target_compile_definitions(qliccore PRIVATE QLIC_NO_MAIN)
target_link_libraries(qliccore PRIVATE Threads::Threads)
if(TARGET qlic_wimlib)
  target_compile_definitions(qliccore PRIVATE QLIC_HAVE_WIMLIB)
  target_link_libraries(qliccore PRIVATE qlic_wimlib)
endif()
if(WIN32)
  qlic_windows_target(qliccore)
endif()

if(WIN32 AND QLIC_BUILD_CLI)
  add_executable(qlic
    codec/src/qlic.c
    codec/src/input.c
    codec/src/stream.c
    codec/src/parallel.c
    codec/src/lzms.c
    ${QLIC_MAP_SOURCES}
    ${QLIC_CLI_RES})
  qlic_windows_target(qlic)
  target_include_directories(qlic PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jxl/include")
  target_link_libraries(qlic PRIVATE
    Threads::Threads windowscodecs ole32 cabinet shell32)
  if(QLIC_BUNDLED_IMAGE_CODECS)
    add_custom_command(TARGET qlic POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/image-codecs"
        "$<TARGET_FILE_DIR:qlic>/image-codecs")
  endif()
else()
  if(QLIC_BUILD_CLI)
    find_package(PNG REQUIRED)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(QLIC_WEBP IMPORTED_TARGET libwebp)
      pkg_check_modules(QLIC_JXL IMPORTED_TARGET libjxl)
      pkg_check_modules(QLIC_AVIF IMPORTED_TARGET libavif)
      pkg_check_modules(QLIC_TIFF IMPORTED_TARGET libtiff-4)
    endif()
  endif()
endif()

add_library(qlic_static STATIC codec/src/qlic_api.c $<TARGET_OBJECTS:qliccore>)
set_target_properties(qlic_static PROPERTIES
  OUTPUT_NAME qlic_static
  EXPORT_NAME qlic_static)
add_library(qlic::qlic_static ALIAS qlic_static)
target_include_directories(qlic_static PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/codec/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(qlic_static PUBLIC QLIC_STATIC)
target_link_libraries(qlic_static PRIVATE Threads::Threads)
if(TARGET qlic_wimlib)
  target_link_libraries(qlic_static PUBLIC qlic_wimlib)
endif()
if(WIN32)
  qlic_windows_target(qlic_static)
  target_link_libraries(qlic_static PRIVATE cabinet)
endif()

add_library(qlic_library SHARED codec/src/qlic_api.c ${QLIC_LIBRARY_RES} $<TARGET_OBJECTS:qliccore>)
set_target_properties(qlic_library PROPERTIES
  OUTPUT_NAME qlic
  EXPORT_NAME qlic
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}"
  C_VISIBILITY_PRESET hidden)
add_library(qlic::qlic ALIAS qlic_library)
target_include_directories(qlic_library PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/codec/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_definitions(qlic_library PRIVATE QLIC_API_BUILD)
target_link_libraries(qlic_library PRIVATE Threads::Threads)
if(TARGET qlic_wimlib)
  target_link_libraries(qlic_library PRIVATE qlic_wimlib)
endif()
if(WIN32)
  qlic_windows_target(qlic_library QLIC_RESOURCE_DLL)
  target_link_libraries(qlic_library PRIVATE cabinet)
endif()

if(QLIC_LIBFUZZER)
  set(QLIC_FUZZ_FLAGS
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer)
  target_compile_options(qliccore PRIVATE ${QLIC_FUZZ_FLAGS})
  target_compile_options(qlic_static PRIVATE ${QLIC_FUZZ_FLAGS})
  target_link_options(qlic_static PRIVATE ${QLIC_FUZZ_FLAGS})
  add_executable(qlic-fuzz-decode tests/fuzz_decode.c)
  target_compile_options(qlic-fuzz-decode PRIVATE ${QLIC_FUZZ_FLAGS})
  target_link_options(qlic-fuzz-decode PRIVATE ${QLIC_FUZZ_FLAGS})
  target_link_libraries(qlic-fuzz-decode PRIVATE qlic_static)
endif()

if(QLIC_BUILD_EXAMPLES)
  foreach(name IN ITEMS encode decode)
    add_executable(qlic-example-${name} examples/c/${name}.c)
    target_compile_definitions(qlic-example-${name} PRIVATE QLIC_STATIC)
    target_link_libraries(qlic-example-${name} PRIVATE qlic_static)
  endforeach()
endif()

function(qlic_benchmark target source)
  add_executable(${target}
    ${source}
    codec/src/stream.c
    codec/src/parallel.c
    ${QLIC_MAP_SOURCES})
  target_include_directories(${target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")
  target_compile_definitions(${target} PRIVATE QLIC_BENCHMARK_TRIAL ${ARGN})
  target_link_libraries(${target} PRIVATE Threads::Threads)
  if(WIN32)
    qlic_windows_target(${target})
  endif()
endfunction()

if(QLIC_BENCHMARK_TRIAL)
  if(TARGET qlic)
    target_compile_definitions(qlic PRIVATE QLIC_BENCHMARK_TRIAL)
  endif()
  qlic_benchmark(qlic-mode-trial-bench
    benchmark/tools/qlic_mode_trial_bench.c)
  qlic_benchmark(qlic-context-cache-replay
    benchmark/tools/qlic_context_cache_replay.c)
  qlic_benchmark(qlic-gradient-topology-replay
    benchmark/tools/qlic_context_cache_replay.c
    QLIC_STREAM_TRACE
    QLIC_GRADIENT_TOPOLOGY_REPLAY)

  add_executable(qlic-wide-memory-probe
    benchmark/tools/qlic_wide_memory_probe.c)
  target_link_libraries(qlic-wide-memory-probe PRIVATE qlic_static)
endif()

if(NOT WIN32 AND QLIC_BUILD_CLI)
  add_executable(qlic codec/src/cli_posix.c)
  target_link_libraries(qlic PRIVATE qlic_static PNG::PNG)
  foreach(codec IN ITEMS WEBP JXL AVIF TIFF)
    if(TARGET PkgConfig::QLIC_${codec})
      target_link_libraries(qlic PRIVATE PkgConfig::QLIC_${codec})
      target_compile_definitions(qlic PRIVATE QLIC_HAVE_${codec})
    endif()
  endforeach()
endif()

if(WIN32 AND QLIC_BUILD_WINDOWS_APPS)
  add_executable(qlic-gui WIN32 codec/src/gui.c codec/src/win_util.c ${QLIC_GUI_RES})
  qlic_windows_unicode_target(qlic-gui)
  target_link_libraries(qlic-gui PRIVATE qlic_static comctl32 comdlg32 dwmapi
    shell32 user32 gdi32 msimg32 mscms windowscodecs ole32)

  add_library(qlic-wic SHARED codec/src/wic_codec.c codec/src/qlic-wic.def ${QLIC_WIC_RES})
  qlic_windows_unicode_target(qlic-wic QLIC_RESOURCE_DLL)
  target_link_libraries(qlic-wic PRIVATE qlic_static windowscodecs ole32 cabinet shell32 advapi32 user32 wintrust)
endif()

if(BUILD_TESTING)
  set(QLIC_TEST_TARGETS
    qlic-api-test
    qlic-api-shared-test
    qlic-encode-candidate-test
    qlic-wide-fuzz-test
    qlic-hdr-fuzz-test
    qlic-planar-med-fuzz-test
    qlic-tile-palette-fuzz-test
    qlic-cpal-planar-fuzz-test
    qlic-mode-tiles-fuzz-test
    qlic-compat-test
    qlic-negative-vectors-test
    qlic-generate-wide-fixtures
    qlic-lzms-robust-test
    qlic-parallel-test
    qlic-stream-test)

  add_executable(qlic-api-test tests/api_test.c)
  target_compile_definitions(qlic-api-test PRIVATE QLIC_STATIC)
  target_link_libraries(qlic-api-test PRIVATE qlic_static Threads::Threads)

  add_executable(qlic-api-shared-test tests/api_test.c)
  target_link_libraries(qlic-api-shared-test PRIVATE qlic_library Threads::Threads)

  add_executable(qlic-encode-candidate-test tests/encode_candidate_test.c
    codec/src/stream.c codec/src/parallel.c codec/src/lzms.c
    ${QLIC_MAP_SOURCES})
  target_link_libraries(qlic-encode-candidate-test PRIVATE Threads::Threads)
  if(WIN32)
    qlic_windows_target(qlic-encode-candidate-test)
    target_link_libraries(qlic-encode-candidate-test PRIVATE cabinet)
  endif()
  if(TARGET qlic_wimlib)
    target_compile_definitions(qlic-encode-candidate-test PRIVATE QLIC_HAVE_WIMLIB)
    target_link_libraries(qlic-encode-candidate-test PRIVATE qlic_wimlib)
  endif()
  add_test(NAME qlic-encode-empty-candidate COMMAND qlic-encode-candidate-test empty)
  add_test(NAME qlic-encode-palette-boundary COMMAND qlic-encode-candidate-test rgb 4000)
  foreach(layout IN ITEMS rgb rgba gray graya)
    add_test(NAME qlic-encode-large-palette-${layout}
      COMMAND qlic-encode-candidate-test ${layout} 4001)
    set_tests_properties(qlic-encode-large-palette-${layout}
      PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(colors IN ITEMS 1 2 16 256 257)
    add_test(NAME qlic-encode-large-palette-${colors}
      COMMAND qlic-encode-candidate-test rgb 4001 ${colors})
    set_tests_properties(qlic-encode-large-palette-${colors}
      PROPERTIES TIMEOUT 120)
  endforeach()
  set_tests_properties(qlic-encode-palette-boundary PROPERTIES TIMEOUT 120)

  add_executable(qlic-wide-fuzz-test tests/wide_fuzz_test.c)
  target_compile_definitions(qlic-wide-fuzz-test PRIVATE QLIC_STATIC)
  target_link_libraries(qlic-wide-fuzz-test PRIVATE qlic_static Threads::Threads)

  add_executable(qlic-hdr-fuzz-test tests/hdr_fuzz_test.c)
  target_compile_definitions(qlic-hdr-fuzz-test PRIVATE QLIC_STATIC)
  target_link_libraries(qlic-hdr-fuzz-test PRIVATE qlic_static Threads::Threads)

  add_executable(qlic-planar-med-fuzz-test tests/planar_med_fuzz_test.c)
  target_compile_definitions(qlic-planar-med-fuzz-test PRIVATE
    QLIC_STATIC QLIC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(qlic-planar-med-fuzz-test PRIVATE qlic_static)

  add_executable(qlic-tile-palette-fuzz-test
    tests/tile_palette_fuzz_test.c)
  target_compile_definitions(qlic-tile-palette-fuzz-test PRIVATE QLIC_STATIC)
  target_link_libraries(qlic-tile-palette-fuzz-test PRIVATE qlic_static)

  add_executable(qlic-cpal-planar-fuzz-test
    tests/cpal_planar_fuzz_test.c)
  target_compile_definitions(qlic-cpal-planar-fuzz-test PRIVATE QLIC_STATIC)
  target_link_libraries(qlic-cpal-planar-fuzz-test PRIVATE qlic_static)

  add_executable(qlic-mode-tiles-fuzz-test tests/mode_tiles_fuzz_test.c)
  target_compile_definitions(qlic-mode-tiles-fuzz-test PRIVATE
    QLIC_STATIC
    QLIC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    QLIC_RGBA_MODE45_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/retained/mode45-current-streams/0013.qlic")
  target_link_libraries(qlic-mode-tiles-fuzz-test PRIVATE
    qlic_static Threads::Threads)

  add_executable(qlic-compat-test tests/compat_test.c)
  target_include_directories(qlic-compat-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/include")
  target_compile_definitions(qlic-compat-test PRIVATE
    QLIC_STATIC QLIC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(qlic-compat-test PRIVATE qlic_static)

  add_executable(qlic-negative-vectors-test tests/negative_vectors_test.c)
  target_include_directories(qlic-negative-vectors-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/codec/include")
  target_compile_definitions(qlic-negative-vectors-test PRIVATE
    QLIC_STATIC QLIC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  target_link_libraries(qlic-negative-vectors-test PRIVATE qlic_static)

  add_executable(qlic-generate-wide-fixtures
    tests/generate_wide_fixtures.c)
  target_compile_definitions(qlic-generate-wide-fixtures PRIVATE QLIC_STATIC)
  target_link_libraries(qlic-generate-wide-fixtures PRIVATE qlic_static)

  add_executable(qlic-lzms-robust-test
    tests/lzms_robust_test.c codec/src/lzms.c)
  target_include_directories(qlic-lzms-robust-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")

  add_executable(qlic-parallel-test tests/parallel_test.c codec/src/parallel.c)
  target_include_directories(qlic-parallel-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")
  target_link_libraries(qlic-parallel-test PRIVATE Threads::Threads)

  add_executable(qlic-stream-test tests/stream_test.c codec/src/stream.c codec/src/parallel.c ${QLIC_MAP_SOURCES})
  target_include_directories(qlic-stream-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")
  target_compile_definitions(qlic-stream-test PRIVATE
    QLIC_BENCHMARK_TRIAL
    QLIC_MODE54_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/retained/mode37-kodak-waterloo-holdout-streams/0005-kodim05.qlic")
  target_link_libraries(qlic-stream-test PRIVATE Threads::Threads)

  add_test(NAME qlic-api COMMAND qlic-api-test)
  add_test(NAME qlic-api-shared COMMAND qlic-api-shared-test)
  add_test(NAME qlic-wide-fuzz COMMAND qlic-wide-fuzz-test)
  add_test(NAME qlic-hdr-fuzz COMMAND qlic-hdr-fuzz-test)
  add_test(NAME qlic-planar-med-fuzz COMMAND qlic-planar-med-fuzz-test)
  add_test(NAME qlic-tile-palette-fuzz COMMAND qlic-tile-palette-fuzz-test)
  add_test(NAME qlic-cpal-planar-fuzz COMMAND qlic-cpal-planar-fuzz-test)
  add_test(NAME qlic-mode-tiles-fuzz COMMAND qlic-mode-tiles-fuzz-test)
  add_test(NAME qlic-compat COMMAND qlic-compat-test)
  add_test(NAME qlic-negative-vectors COMMAND qlic-negative-vectors-test)
  add_test(NAME qlic-release-truth
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DPROJECT_VERSION=${PROJECT_VERSION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_release_truth.cmake")
  if(WIN32)
    add_test(NAME qlic-wic-install-contract
      COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_wic_install_contract.cmake")
  endif()
  add_test(NAME qlic-conformance-manifest
    COMMAND "${CMAKE_COMMAND}"
      "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/manifest.json"
      "-DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
      "-DPROFILES=${CMAKE_CURRENT_SOURCE_DIR}/docs/profiles.json"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_manifest.cmake")
  add_test(NAME qlic-conformance-lock
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DLOCK=${CMAKE_CURRENT_SOURCE_DIR}/docs/conformance.lock.json"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_conformance_lock.cmake")
  add_test(NAME qlic-lzms-robust
    COMMAND qlic-lzms-robust-test
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cpalette-lzms.qlic")
  add_test(NAME qlic-tile-palette-lzms-robust
    COMMAND qlic-lzms-robust-test
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tile-palette-lzms.qlic")
  add_test(NAME qlic-parallel COMMAND qlic-parallel-test)
  add_test(NAME qlic-stream COMMAND qlic-stream-test)
  if(TARGET qlic-gradient-topology-replay)
    set(QLIC_GRADIENT_TOPOLOGY_SMOKE_MANIFEST
      "${CMAKE_CURRENT_BINARY_DIR}/gradient-topology-smoke.txt")
    file(WRITE "${QLIC_GRADIENT_TOPOLOGY_SMOKE_MANIFEST}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/retained/mode53-post-alpha-current-streams/0020.qlic\n")
    add_test(NAME qlic-gradient-topology-replay
      COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:qlic-gradient-topology-replay>"
        "-DMANIFEST=${QLIC_GRADIENT_TOPOLOGY_SMOKE_MANIFEST}"
        "-DTRACE=${CMAKE_CURRENT_BINARY_DIR}/gradient-topology-smoke.trace"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_gradient_topology_replay.cmake")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      add_test(NAME qlic-gradient-topology-summary
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/benchmark/tools/test_summarize_gradient_topology.py")
    endif()
  endif()
  find_program(QLIC_CARGO_EXECUTABLE NAMES cargo)
  if(QLIC_CARGO_EXECUTABLE)
    add_test(NAME qlic-rust-decoder
      COMMAND "${QLIC_CARGO_EXECUTABLE}" test
        --manifest-path
          "${CMAKE_CURRENT_SOURCE_DIR}/rust/qlic-decoder/Cargo.toml"
        --locked
        --target-dir "${CMAKE_CURRENT_BINARY_DIR}/rust-target")
  endif()
  if(UNIX AND NOT APPLE)
    add_test(NAME qlic-exports
      COMMAND "${CMAKE_COMMAND}"
        "-DNM=${CMAKE_NM}"
        "-DLIBRARY=$<TARGET_FILE:qlic_library>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exports.cmake")
  endif()

  if(WIN32)
    foreach(target IN ITEMS
        qlic-api-test
        qlic-api-shared-test
        qlic-wide-fuzz-test
        qlic-hdr-fuzz-test
        qlic-planar-med-fuzz-test
        qlic-mode-tiles-fuzz-test)
      qlic_windows_target(${target})
    endforeach()

    add_executable(qlic-win-util-test tests/win_util_test.c codec/src/win_util.c)
    target_include_directories(qlic-win-util-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")
    qlic_windows_unicode_target(qlic-win-util-test)
    target_link_libraries(qlic-win-util-test PRIVATE shell32)

    if(TARGET qlic-gui)
      add_executable(qlic-gui-test tests/gui_test.c)
      qlic_windows_unicode_target(qlic-gui-test)
      target_link_libraries(qlic-gui-test PRIVATE user32)
      add_dependencies(qlic-gui-test qlic-gui)
      list(APPEND QLIC_TEST_TARGETS qlic-gui-test)
      add_test(NAME qlic-gui-controls
        COMMAND qlic-gui-test "$<TARGET_FILE:qlic-gui>")
      add_test(NAME qlic-gui-lossy-warning
        COMMAND qlic-gui-test "$<TARGET_FILE:qlic-gui>"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/lossy.jpg")
      add_test(NAME qlic-gui-view-native
        COMMAND qlic-gui-test "$<TARGET_FILE:qlic-gui>"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native.qlic" "64 × 64")
      add_test(NAME qlic-gui-view-animation
        COMMAND qlic-gui-test "$<TARGET_FILE:qlic-gui>"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation.qlic" "2 × 2")
      add_test(NAME qlic-gui-view-wide24
        COMMAND qlic-gui-test "$<TARGET_FILE:qlic-gui>"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wide-u32-24-rgb.qlic" "2 × 2")
      add_test(NAME qlic-gui-view-hdr10
        COMMAND qlic-gui-test "$<TARGET_FILE:qlic-gui>"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hdr-u16-10-pq-rgb.qlic" "2 × 2")
      add_test(NAME qlic-gui-view-hlg
        COMMAND qlic-gui-test "$<TARGET_FILE:qlic-gui>"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hdr-u16-10-hlg-rgb.qlic" "2 × 2")
      set_tests_properties(qlic-gui-controls qlic-gui-lossy-warning
        qlic-gui-view-native
        qlic-gui-view-animation qlic-gui-view-wide24 qlic-gui-view-hdr10
        qlic-gui-view-hlg PROPERTIES TIMEOUT 15)
    endif()

    add_executable(qlic-input-test tests/input_test.c codec/src/input.c)
    target_include_directories(qlic-input-test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/codec/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/jxl/include")
    qlic_windows_unicode_target(qlic-input-test)
    if(QLIC_BUNDLED_IMAGE_CODECS)
      target_compile_definitions(qlic-input-test PRIVATE
        QLIC_TEST_BUNDLED_IMAGE_CODECS)
      add_custom_command(TARGET qlic-input-test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/runtime/image-codecs"
          "$<TARGET_FILE_DIR:qlic-input-test>/image-codecs")
    endif()

    add_executable(qlic-lzms-test tests/lzms_test.c codec/src/lzms.c)
    target_include_directories(qlic-lzms-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/codec/src")
    qlic_windows_target(qlic-lzms-test)
    target_link_libraries(qlic-lzms-test PRIVATE cabinet)

    list(APPEND QLIC_TEST_TARGETS
      qlic-win-util-test qlic-input-test qlic-lzms-test)
    add_test(NAME qlic-win-util COMMAND qlic-win-util-test)
    add_test(NAME qlic-input
      COMMAND qlic-input-test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input")
    add_test(NAME qlic-lzms
      COMMAND qlic-lzms-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cpalette-lzms.qlic"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gray-model-lzms.qlic"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rgb-lzms.qlic")
    if(TARGET qlic)
      find_program(QLIC_POWERSHELL NAMES pwsh powershell REQUIRED)
      set(QLIC_INPUT_CLI_OPTIONS)
      if(NOT QLIC_BUNDLED_IMAGE_CODECS)
        list(APPEND QLIC_INPUT_CLI_OPTIONS -SkipBundledImageCodecs)
      endif()
      add_test(NAME qlic-input-cli
        COMMAND "${QLIC_POWERSHELL}" -NoProfile -File
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/input_cli_test.ps1"
          -Qlic "$<TARGET_FILE:qlic>"
          -Fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input"
          -Output "${CMAKE_CURRENT_BINARY_DIR}/input-cli-test"
          ${QLIC_INPUT_CLI_OPTIONS})
    endif()
    if(TARGET qlic-wic)
      add_executable(qlic-wic-smoke tests/wic_smoke.c)
      qlic_windows_unicode_target(qlic-wic-smoke)
      target_link_libraries(qlic-wic-smoke PRIVATE windowscodecs ole32)
      add_dependencies(qlic-wic-smoke qlic-wic)
      add_test(NAME qlic-wic-direct COMMAND qlic-wic-smoke --self-test)
      add_test(NAME qlic-wic-hdr8-direct
        COMMAND qlic-wic-smoke --direct-hdr8
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/described-u16-8-srgb-rgb.qlic")
      add_test(NAME qlic-wic-hdr10-direct
        COMMAND qlic-wic-smoke --direct-hdr10
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hdr-u16-10-pq-rgb.qlic")
      add_test(NAME qlic-wic-unsupported-precision-direct
        COMMAND qlic-wic-smoke --direct-reject
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hdr-u16-12-pq-rgba.qlic")
      add_test(NAME qlic-wic-hlg-not-hdr10
        COMMAND qlic-wic-smoke --direct-reject
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hdr-u16-10-hlg-rgb.qlic")
      if(TARGET qlic)
        add_test(NAME qlic-wic-hdr16-direct
          COMMAND qlic-wic-smoke --direct-hdr16
            "${CMAKE_CURRENT_BINARY_DIR}/input-cli-test/described16-rgba.qlic")
        set_tests_properties(qlic-wic-hdr16-direct PROPERTIES
          DEPENDS qlic-input-cli)
        add_test(NAME qlic-wic-hdr16-premultiplied-direct
          COMMAND qlic-wic-smoke --direct-hdr16-premultiplied
            "${CMAKE_CURRENT_BINARY_DIR}/input-cli-test/described16-premultiplied.qlic")
        set_tests_properties(qlic-wic-hdr16-premultiplied-direct PROPERTIES
          DEPENDS qlic-input-cli)
        add_test(NAME qlic-wic-metadata-direct
          COMMAND qlic-wic-smoke --direct-metadata
            "${CMAKE_CURRENT_BINARY_DIR}/input-cli-test/wic-metadata.qlic")
        set_tests_properties(qlic-wic-metadata-direct PROPERTIES
          DEPENDS qlic-input-cli)
        add_test(NAME qlic-wic-resolution-direct
          COMMAND qlic-wic-smoke --direct-resolution
            "${CMAKE_CURRENT_BINARY_DIR}/input-cli-test/wic-resolution.qlic")
        set_tests_properties(qlic-wic-resolution-direct PROPERTIES
          DEPENDS qlic-input-cli)
        add_test(NAME qlic-wic-wide-direct
          COMMAND qlic-wic-smoke --direct-wide
            "${CMAKE_CURRENT_BINARY_DIR}/input-cli-test/dense16.qlic")
        set_tests_properties(qlic-wic-wide-direct PROPERTIES
          DEPENDS qlic-input-cli)
      endif()
    endif()
  elseif(TARGET qlic)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cli-test")
    add_test(NAME qlic-linux-pack-png
      COMMAND qlic pack
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/base.png"
        "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-png.qlic"
        --threads all)
    if(TARGET qlic_wimlib)
      add_test(NAME qlic-linux-emits-lzms
        COMMAND "${CMAKE_COMMAND}"
          "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-png.qlic"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_lzms_output.cmake")
      set_tests_properties(qlic-linux-emits-lzms PROPERTIES
        DEPENDS qlic-linux-pack-png)
      add_test(NAME qlic-linux-roundtrip-own-lzms
        COMMAND qlic unpack
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-png.qlic"
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-png-roundtrip.png")
      set_tests_properties(qlic-linux-roundtrip-own-lzms PROPERTIES
        DEPENDS qlic-linux-emits-lzms)
    endif()
    add_test(NAME qlic-linux-pack-bmp
      COMMAND qlic pack
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/base.bmp"
        "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-bmp.qlic")
    add_test(NAME qlic-linux-unpack-lzms
      COMMAND qlic unpack
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rgb-lzms.qlic"
        "${CMAKE_CURRENT_BINARY_DIR}/cli-test/rgb-lzms.png")
    add_test(NAME qlic-linux-batch
      COMMAND qlic batch
        "${CMAKE_CURRENT_BINARY_DIR}/cli-test/batch"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/base.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/base.bmp"
        --threads 4)
    if(TARGET PkgConfig::QLIC_AVIF)
      add_test(NAME qlic-linux-pack-avif
        COMMAND qlic pack
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/lossless.avif"
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-avif.qlic")
      add_test(NAME qlic-linux-pack-lossy-avif
        COMMAND "${CMAKE_COMMAND}"
          "-DQLIC=$<TARGET_FILE:qlic>"
          "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/lossy.avif"
          "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/cli-test/lossy-avif.qlic"
          "-DDECODED=${CMAKE_CURRENT_BINARY_DIR}/cli-test/lossy-avif.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_lossy_input.cmake")
    endif()
    if(TARGET PkgConfig::QLIC_TIFF)
      add_test(NAME qlic-linux-pack-tiff
        COMMAND qlic pack
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/lossless.tiff"
          "${CMAKE_CURRENT_BINARY_DIR}/cli-test/base-tiff.qlic")
      add_test(NAME qlic-linux-pack-lossy-tiff
        COMMAND "${CMAKE_COMMAND}"
          "-DQLIC=$<TARGET_FILE:qlic>"
          "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/input/lossy.tiff"
          "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/cli-test/lossy-tiff.qlic"
          "-DDECODED=${CMAKE_CURRENT_BINARY_DIR}/cli-test/lossy-tiff.png"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_lossy_input.cmake")
    endif()
  endif()

  if(TARGET qlic)
    add_test(NAME qlic-cli-help COMMAND qlic --help)
    add_test(NAME qlic-cli-version COMMAND qlic version)
    add_test(NAME qlic-cli-info-native
      COMMAND qlic info "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native.qlic")
    add_test(NAME qlic-cli-reject-unknown COMMAND qlic unknown)
    add_test(NAME qlic-cli-reject-invalid-threads
      COMMAND qlic info "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native.qlic" --threads invalid)
    add_test(NAME qlic-cli-reject-info-threads
      COMMAND qlic info "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native.qlic" --threads 1)
    add_test(NAME qlic-cli-reject-extra-argument COMMAND qlic version extra)
    set_tests_properties(qlic-cli-version PROPERTIES PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}")
    if(WIN32)
      add_test(NAME qlic-cli-info-json
        COMMAND qlic info "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native.qlic" --json)
      add_test(NAME qlic-cli-verify-native
        COMMAND qlic verify "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native.qlic")
      add_test(NAME qlic-cli-verify-hdr-json
        COMMAND qlic verify "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hdr-u16-12-pq-rgba.qlic" --json)
      set_tests_properties(qlic-cli-info-native PROPERTIES
        PASS_REGULAR_EXPRESSION
          "native-mode=[0-9][0-9]* native-transform=[0-9][0-9]* native-tile-log=[0-9][0-9]* native-adaptation=[0-9][0-9]*")
      set_tests_properties(qlic-cli-info-json PROPERTIES
        PASS_REGULAR_EXPRESSION "\"native\":\\{")
      set_tests_properties(qlic-cli-verify-native PROPERTIES
        PASS_REGULAR_EXPRESSION "valid QLIC 64x64")
      set_tests_properties(qlic-cli-verify-hdr-json PROPERTIES
        PASS_REGULAR_EXPRESSION "\"valid\":true")
    else()
      set_tests_properties(qlic-cli-info-native PROPERTIES
        PASS_REGULAR_EXPRESSION "64x64 frames=1 animated=0")
    endif()
    set_tests_properties(
      qlic-cli-reject-unknown
      qlic-cli-reject-invalid-threads
      qlic-cli-reject-info-threads
      qlic-cli-reject-extra-argument
      PROPERTIES WILL_FAIL TRUE)
  endif()
endif()

set(QLIC_LINK_TARGETS qlic_static qlic_library)
foreach(target IN ITEMS
    qlic-example-encode qlic-example-decode qlic
    qlic-mode-trial-bench qlic-context-cache-replay
    qlic-gradient-topology-replay qlic-wide-memory-probe
    qlic-gui qlic-wic qlic-wic-smoke)
  if(TARGET ${target})
    list(APPEND QLIC_LINK_TARGETS ${target})
  endif()
endforeach()
if(BUILD_TESTING)
  list(APPEND QLIC_LINK_TARGETS ${QLIC_TEST_TARGETS})
endif()
set(QLIC_COMPILE_TARGETS qliccore ${QLIC_LINK_TARGETS})

if(QLIC_NATIVE)
  if(MSVC)
    set(QLIC_NATIVE_COMPILE_OPTION /clang:-march=native)
  else()
    set(QLIC_NATIVE_COMPILE_OPTION -march=native)
  endif()
  foreach(target IN LISTS QLIC_COMPILE_TARGETS)
    target_compile_options(${target} PRIVATE ${QLIC_NATIVE_COMPILE_OPTION})
  endforeach()
endif()

if(QLIC_PGO STREQUAL "GENERATE")
  if(MSVC)
    set(QLIC_PGO_COMPILE_OPTIONS /clang:-fprofile-instr-generate)
  else()
    set(QLIC_PGO_COMPILE_OPTIONS -fprofile-instr-generate)
    set(QLIC_PGO_LINK_OPTIONS -fprofile-instr-generate)
  endif()
elseif(QLIC_PGO STREQUAL "USE")
  if(MSVC)
    set(QLIC_PGO_COMPILE_OPTIONS
      "/clang:-fprofile-instr-use=${QLIC_PGO_PROFILE}"
      /clang:-Wno-profile-instr-unprofiled)
  else()
    set(QLIC_PGO_COMPILE_OPTIONS
      "-fprofile-instr-use=${QLIC_PGO_PROFILE}"
      -Wno-profile-instr-unprofiled)
  endif()
endif()
if(QLIC_PGO_COMPILE_OPTIONS)
  foreach(target IN LISTS QLIC_COMPILE_TARGETS)
    target_compile_options(${target} PRIVATE ${QLIC_PGO_COMPILE_OPTIONS})
  endforeach()
endif()
if(QLIC_PGO_LINK_OPTIONS)
  foreach(target IN LISTS QLIC_LINK_TARGETS)
    target_link_options(${target} PRIVATE ${QLIC_PGO_LINK_OPTIONS})
  endforeach()
endif()

if(MSVC)
  if(QLIC_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_property(TARGET ${QLIC_COMPILE_TARGETS} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  else()
    set_property(TARGET ${QLIC_COMPILE_TARGETS} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  foreach(target IN LISTS QLIC_COMPILE_TARGETS)
    # release archives are reproducible only when compiler output is too
    target_compile_options(${target} PRIVATE
      /W4 /WX /Brepro
      "$<$<CONFIG:Debug>:/Od>"
      "$<$<NOT:$<CONFIG:Debug>>:/O2>"
      "$<$<NOT:$<CONFIG:Debug>>:/Oi>"
      "$<$<NOT:$<CONFIG:Debug>>:/Ot>"
      "$<$<NOT:$<CONFIG:Debug>>:/Gy>"
      "$<$<NOT:$<CONFIG:Debug>>:/Gw>")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(${target} PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:/clang:-O3>"
        /clang:-Wall /clang:-Wextra /clang:-Wconversion
        /clang:-Wno-unsafe-buffer-usage)
    else()
      target_compile_options(${target} PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:/GL>" /wd5105)
    endif()
  endforeach()
  foreach(target IN LISTS QLIC_LINK_TARGETS)
    target_link_options(${target} PRIVATE
      "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF>"
      "$<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_link_options(${target} PRIVATE /timestamp:0)
    else()
      target_link_options(${target} PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:/LTCG>" /Brepro)
    endif()
  endforeach()
  if(TARGET qlic AND CMAKE_C_COMPILER_ID MATCHES "Clang" AND
     NOT QLIC_SANITIZE)
    target_compile_options(qlic PRIVATE
      "$<$<NOT:$<CONFIG:Debug>>:/clang:-flto=thin>")
  endif()
else()
  foreach(target IN LISTS QLIC_COMPILE_TARGETS)
    target_compile_options(${target} PRIVATE
      "$<$<CONFIG:Debug>:-O0>"
      "$<$<NOT:$<CONFIG:Debug>>:-O3>"
      -Wall -Wextra)
  endforeach()
endif()

if(QLIC_SANITIZE)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "QLIC_SANITIZE requires Clang")
  endif()
  if(MSVC)
    get_filename_component(QLIC_CLANG_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
    file(GLOB QLIC_CLANG_RT_DIRS "${QLIC_CLANG_BIN}/../lib/clang/*/lib/windows")
    list(SORT QLIC_CLANG_RT_DIRS COMPARE NATURAL ORDER DESCENDING)
    list(LENGTH QLIC_CLANG_RT_DIRS QLIC_CLANG_RT_COUNT)
    if(QLIC_CLANG_RT_COUNT EQUAL 0)
      message(FATAL_ERROR "Could not find the LLVM sanitizer runtime")
    endif()
    list(GET QLIC_CLANG_RT_DIRS 0 QLIC_CLANG_RT_DIR)
    set(QLIC_ASAN_DYNAMIC "${QLIC_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.lib")
    set(QLIC_ASAN_THUNK "${QLIC_CLANG_RT_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
    set(QLIC_ASAN_DLL "${QLIC_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT EXISTS "${QLIC_ASAN_DYNAMIC}" OR NOT EXISTS "${QLIC_ASAN_THUNK}" OR NOT EXISTS "${QLIC_ASAN_DLL}")
      message(FATAL_ERROR "Could not find the LLVM sanitizer runtime files")
    endif()
    foreach(target IN LISTS QLIC_COMPILE_TARGETS)
      target_compile_options(${target} PRIVATE
        /fsanitize=address /Zi /Oy- /clang:-O1)
    endforeach()
    foreach(target IN LISTS QLIC_LINK_TARGETS)
      if(NOT target STREQUAL "qlic_static")
        target_link_directories(${target} PRIVATE "${QLIC_CLANG_RT_DIR}")
        target_link_libraries(${target} PRIVATE "${QLIC_ASAN_DYNAMIC}")
        target_link_options(${target} PRIVATE "/WHOLEARCHIVE:${QLIC_ASAN_THUNK}")
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${QLIC_ASAN_DLL}" "$<TARGET_FILE_DIR:${target}>")
      endif()
    endforeach()
  else()
    foreach(target IN LISTS QLIC_COMPILE_TARGETS)
      target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -O1)
    endforeach()
    foreach(target IN LISTS QLIC_LINK_TARGETS)
      if(NOT target STREQUAL "qlic_static")
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
      endif()
    endforeach()
  endif()
endif()

set(QLIC_RUNTIME_TARGETS)
if(TARGET qlic)
  list(APPEND QLIC_RUNTIME_TARGETS qlic)
endif()
if(WIN32 AND QLIC_BUILD_WINDOWS_APPS)
  list(APPEND QLIC_RUNTIME_TARGETS qlic-gui qlic-wic)
endif()
if(QLIC_RUNTIME_TARGETS)
  install(TARGETS ${QLIC_RUNTIME_TARGETS}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
if(WIN32 AND TARGET qlic AND QLIC_BUNDLED_IMAGE_CODECS)
  install(DIRECTORY runtime/image-codecs
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
install(TARGETS qlic_library qlic_static
  EXPORT qlicTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES codec/include/qlic/qlic.h
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/qlic")
install(EXPORT qlicTargets
  FILE qlicTargets.cmake
  NAMESPACE qlic::
  DESTINATION "${QLIC_CMAKE_INSTALL_DIR}")
install(FILES
  "${QLIC_GENERATED_DIR}/qlicConfig.cmake"
  "${QLIC_GENERATED_DIR}/qlicConfigVersion.cmake"
  DESTINATION "${QLIC_CMAKE_INSTALL_DIR}")
install(FILES "${QLIC_GENERATED_DIR}/qlic.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
install(FILES README.md LICENSE NOTICE SECURITY.md SUPPORT.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic")
install(FILES third_party/lzms/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic/third_party/lzms")
install(FILES third_party/wimlib/README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic/third_party/wimlib")
install(FILES
  docs/sdk.md
  docs/profiles.md
  docs/profiles.json
  docs/format.md
  docs/core-still.md
  docs/hdr-format.md
  docs/predictor-math.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic/docs")
if(WIN32 AND TARGET qlic-wic)
  install(FILES scripts/install-wic.cmd scripts/install-wic.ps1
    scripts/uninstall-wic.cmd scripts/uninstall-wic.ps1
    DESTINATION "${CMAKE_INSTALL_DATADIR}/qlic")
endif()
