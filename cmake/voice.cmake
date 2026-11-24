enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE AND NOT OPENSSL_ROOT_DIR)
  foreach(prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
    if(EXISTS ${prefix}/include/openssl/evp.h)
      set(OPENSSL_ROOT_DIR ${prefix})
      break()
    endif()
  endforeach()
endif()

find_package(OpenSSL 3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH
    SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  OVERRIDE_FIND_PACKAGE)

set(DISABLE_GREASE ON CACHE BOOL "" FORCE)
set(MLS_CXX_NAMESPACE mlspp CACHE STRING "" FORCE)
set(TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mlspp
  GIT_REPOSITORY https://github.com/cisco/mlspp.git
  GIT_TAG 1cc50a124a3bc4e143a787ec934280dc70c1034d
  EXCLUDE_FROM_ALL)

FetchContent_Declare(libdave
  GIT_REPOSITORY https://github.com/discord/libdave.git
  GIT_TAG b5463c443df74db75a713c775b09a847621e6d62
  SOURCE_SUBDIR none)

FetchContent_Declare(rnnoise
  URL https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
  URL_HASH
    SHA256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
  SOURCE_SUBDIR none)

FetchContent_MakeAvailable(nlohmann_json mlspp libdave rnnoise)

# mlspp builds with -Werror, and openssl 4.1 deprecates calls it makes.
foreach(target mlspp hpke bytes tls_syntax mls_ds mls_vectors)
  if(TARGET ${target})
    target_compile_options(${target} PRIVATE -Wno-deprecated-declarations)
  endif()
endforeach()

set(rnnoise ${rnnoise_SOURCE_DIR})
add_library(quarrel_rnnoise STATIC
  ${rnnoise}/src/celt_lpc.c
  ${rnnoise}/src/denoise.c
  ${rnnoise}/src/kiss_fft.c
  ${rnnoise}/src/nnet.c
  ${rnnoise}/src/nnet_default.c
  ${rnnoise}/src/parse_lpcnet_weights.c
  ${rnnoise}/src/pitch.c
  ${rnnoise}/src/rnn.c
  ${rnnoise}/src/rnnoise_data.c
  ${rnnoise}/src/rnnoise_tables.c)
set_target_properties(quarrel_rnnoise PROPERTIES C_STANDARD 11)
target_include_directories(quarrel_rnnoise
  PUBLIC ${rnnoise}/include
  PRIVATE ${rnnoise}/src ${CMAKE_CURRENT_LIST_DIR}/rnnoise)
target_compile_options(quarrel_rnnoise PRIVATE -w)

set(dave ${libdave_SOURCE_DIR}/cpp)
file(GLOB_RECURSE dave_sources CONFIGURE_DEPENDS ${dave}/src/*.cpp)
list(FILTER dave_sources EXCLUDE REGEX
  ".*(persisted_key_pair(_apple|_win|_generic)?\\.cpp|detail/|_wasm\\.cpp|boringssl_cryptor).*")
list(APPEND dave_sources ${dave}/src/mls/persisted_key_pair_null.cpp)
list(REMOVE_DUPLICATES dave_sources)

add_library(quarrel_dave STATIC ${dave_sources})
target_include_directories(quarrel_dave
  PUBLIC ${dave}/includes PRIVATE ${dave}/src)
target_compile_definitions(quarrel_dave
  PUBLIC DAVE_STATIC PRIVATE WITH_OPENSSL3)
target_link_libraries(quarrel_dave PRIVATE mlspp OpenSSL::Crypto)
