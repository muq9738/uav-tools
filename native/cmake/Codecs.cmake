include(FetchContent)
# Public upstreams, pinned release commits, Apache-2.0. Static, no external encoder binaries.
set(DRACO_TESTS OFF CACHE BOOL "" FORCE)
set(DRACO_JS_GLUE OFF CACHE BOOL "" FORCE)
set(DRACO_TRANSCODER_SUPPORTED OFF CACHE BOOL "" FORCE)
set(DRACO_ANIMATION_ENCODING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(draco
  GIT_REPOSITORY https://github.com/google/draco.git
  GIT_TAG 1.5.7
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(draco)
if(NOT draco_POPULATED)
  FetchContent_Populate(draco)
endif()
find_package(Git REQUIRED)
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${draco_SOURCE_DIR} rev-parse HEAD OUTPUT_VARIABLE draco_commit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT draco_commit STREQUAL "8786740086a9f4d83f44aa83badfbea4dce7a1b5")
  message(FATAL_ERROR "Draco 1.5.7 source commit mismatch: ${draco_commit}")
endif()
add_subdirectory(${draco_SOURCE_DIR} ${draco_BINARY_DIR})
FetchContent_Declare(basis_universal
  GIT_REPOSITORY https://github.com/BinomialLLC/basis_universal.git
  GIT_TAG 1.16.4
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(basis_universal)
if(NOT basis_universal_POPULATED)
  FetchContent_Populate(basis_universal)
endif()
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${basis_universal_SOURCE_DIR} rev-parse HEAD OUTPUT_VARIABLE basis_commit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT basis_commit STREQUAL "900e40fb5d2502927360fe2f31762bdbb624455f")
  message(FATAL_ERROR "Basis Universal 1.16.4 source commit mismatch: ${basis_commit}")
endif()
# Upstream 1.16.4 only defines a CLI target and changes directory-wide flags.
# Build exactly its native encoder/transcoder source list as a static library.
set(basis_encoder_sources
  basisu_backend.cpp basisu_basis_file.cpp basisu_comp.cpp basisu_enc.cpp
  basisu_etc.cpp basisu_frontend.cpp basisu_gpu_texture.cpp basisu_pvrtc1_4.cpp
  basisu_resampler.cpp basisu_resample_filters.cpp basisu_ssim.cpp basisu_uastc_enc.cpp
  basisu_bc7enc.cpp jpgd.cpp basisu_kernels_sse.cpp basisu_opencl.cpp pvpngreader.cpp)
list(TRANSFORM basis_encoder_sources PREPEND "${basis_universal_SOURCE_DIR}/encoder/")
add_library(basis_native STATIC ${basis_encoder_sources}
  ${basis_universal_SOURCE_DIR}/transcoder/basisu_transcoder.cpp
  ${basis_universal_SOURCE_DIR}/zstd/zstd.c)
target_include_directories(basis_native PUBLIC ${basis_universal_SOURCE_DIR})
target_compile_definitions(basis_native PUBLIC BASISD_SUPPORT_KTX2_ZSTD=1 BASISD_SUPPORT_KTX2=1
  BASISU_SUPPORT_SSE=0 BASISU_SUPPORT_OPENCL=0)
target_link_libraries(basis_native PUBLIC Threads::Threads)
if(NOT MSVC)
  target_compile_options(basis_native PRIVATE -fno-strict-aliasing)
endif()
