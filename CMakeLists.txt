cmake_minimum_required(VERSION 3.29)

if(NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
  if(APPLE)
    find_program(CMAKE_C_COMPILER clang
      PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH)
    set(CMAKE_OBJC_COMPILER ${CMAKE_C_COMPILER} CACHE FILEPATH "objc compiler")

    set(CMAKE_CXX_COMPILER /usr/bin/c++ CACHE FILEPATH "c++ compiler")
  else()
    find_program(CMAKE_C_COMPILER clang)
    find_program(CMAKE_CXX_COMPILER clang++)
  endif()
endif()

project(quarrel VERSION 0.0.0 LANGUAGES C)

if(APPLE)
  enable_language(OBJC)
endif()

set(QUARREL_C2Y -std=gnu2y -fdefer-ts)
include(CheckCCompilerFlag)
check_c_compiler_flag(-fdefer-ts QUARREL_HAS_DEFER)
if(NOT QUARREL_HAS_DEFER)
  message(FATAL_ERROR
    "Quarrel is C2y, set CC to clang 22 or later. "
    "A build directory made before C2y keeps its old compiler, run make clean.")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES
  AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

if(PROJECT_IS_TOP_LEVEL AND APPLE
  AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX /Applications CACHE PATH "install prefix" FORCE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
  option(QUARREL_NATIVE "build for the processor of this machine"
    ${PROJECT_IS_TOP_LEVEL})
  option(QUARREL_LTO "optimize across quarrel and its libraries at link time"
    ${PROJECT_IS_TOP_LEVEL})

  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)")
    set(QUARREL_NATIVE_FLAG -mcpu=native)
  else()
    set(QUARREL_NATIVE_FLAG -march=native)
  endif()

  check_c_compiler_flag(${QUARREL_NATIVE_FLAG} QUARREL_HAS_NATIVE)
  if(QUARREL_NATIVE AND QUARREL_HAS_NATIVE AND NOT CMAKE_CROSSCOMPILING)
    add_compile_options(${QUARREL_NATIVE_FLAG})
  endif()

  find_program(QUARREL_LLD ld.lld)
  if(QUARREL_LLD AND NOT CMAKE_LINKER_TYPE)
    set(CMAKE_LINKER_TYPE LLD)
  endif()

  if(QUARREL_LTO AND QUARREL_LLD AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT QUARREL_HAS_LTO LANGUAGES C)
    if(QUARREL_HAS_LTO)
      set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    endif()
  endif()

  add_compile_options(-fno-math-errno -ffunction-sections -fdata-sections)
endif()

find_program(QUARREL_CCACHE ccache)
if(QUARREL_CCACHE)
  foreach(lang C CXX OBJC)
    if(NOT CMAKE_${lang}_COMPILER_LAUNCHER)
      set(CMAKE_${lang}_COMPILER_LAUNCHER ${QUARREL_CCACHE})
    endif()
  endforeach()
endif()

include(FetchContent)
foreach(dep cilo:ciko/cilo civue:ciko/civue cinc:ciko/cinc cima:ciko/cima
    ciphon:ciko/cilent ciren:ciko/ciren hcaptcha-civue:hcaptcha/civue)
  string(REPLACE ":" ";" dep ${dep})
  list(GET dep 0 name)
  list(GET dep 1 repo)
  string(TOUPPER ${name} NAME)
  string(REPLACE "/" "-" checkout ${repo})
  get_filename_component(local ${CMAKE_CURRENT_SOURCE_DIR}/../${checkout} ABSOLUTE)
  if(NOT FETCHCONTENT_SOURCE_DIR_${NAME} AND EXISTS ${local}/CMakeLists.txt)
    set(FETCHCONTENT_SOURCE_DIR_${NAME} ${local})
  endif()
  FetchContent_Declare(${name}
    GIT_REPOSITORY https://code.vexcited.com/${repo}.git
    GIT_TAG main)
endforeach()

set(CIVUE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CIPHON_BINDINGS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cilo civue cinc cima ciphon ciren)
if(NOT TARGET ciphon::curl)
  message(FATAL_ERROR "Quarrel needs libcurl ")
endif()

set(QUARREL_CORE_SOURCES
  src/http.c
  src/images.c
  src/sounds.c
  src/sound_files.c
  src/discord/admin.c
  src/discord/api.c
  src/discord/cdn.c
  src/discord/commands.c
  src/discord/directory.c
  src/discord/apps.c
  src/discord/discovery.c
  src/discord/oauth.c
  src/discord/emoji.c
  src/discord/emoji_table.c
  src/discord/etf.c
  src/discord/events.c
  src/discord/expressions.c
  src/discord/fakestickers.c
  src/discord/gateway.c
  src/discord/gifs.c
  src/discord/markdown.c
  src/discord/permissions.c
  src/discord/remote_auth.c
  src/discord/settings_proto.c
  src/discord/templates.c
  src/discord/session.c
  src/discord/store.c
  src/discord/uploads.c
  src/discord/util.c
  src/voice/dave.c
  src/voice/denoise.c
  src/voice/devices.c
  src/voice/mictest.c
  src/voice/rtp.c
  src/voice/voice.c)

set(QUARREL_DARWIN_SOURCES
  src/platform/darwin/audio.m
  src/platform/darwin/camera.m
  src/platform/darwin/notify.m
  src/platform/darwin/pending.m
  src/platform/darwin/qr.m
  src/platform/darwin/storage.m
  src/platform/darwin/system.m
  src/platform/darwin/timer.m
  src/platform/darwin/websocket.m)

set(QUARREL_LINUX_SOURCES
  src/platform/linux/audio.c
  src/platform/linux/camera.c
  src/platform/linux/loop.c
  src/platform/linux/memory.c
  src/platform/linux/notify.c
  src/platform/linux/pipe.c
  src/platform/linux/storage.c
  src/platform/linux/system.c
  src/platform/linux/websocket.c
  src/platform/qr.c)

set(QUARREL_WINDOWS_SOURCES
  src/platform/windows/audio.c
  src/platform/windows/camera.c
  src/platform/windows/loop.c
  src/platform/windows/notify.c
  src/platform/windows/posix.c
  src/platform/windows/storage.c
  src/platform/windows/system.c
  src/platform/windows/websocket.c
  src/platform/windows/win.c
  src/platform/qr.c)

if(APPLE)
  add_library(quarrel_core STATIC
    ${QUARREL_CORE_SOURCES} ${QUARREL_DARWIN_SOURCES})
  target_compile_options(quarrel_core PRIVATE
    $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(quarrel_core PUBLIC
    "-framework Foundation" "-framework Security" "-framework ImageIO"
    "-framework CoreGraphics" "-framework CoreFoundation" "-framework CoreImage"
    "-framework AudioToolbox" "-framework AVFoundation"
    "-framework CoreAudio" "-framework CoreMedia" "-framework CoreVideo"
    "-framework Metal" "-framework AppKit" "-framework Network"
    "-framework UserNotifications" z)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  find_package(CURL REQUIRED)
  pkg_check_modules(QUARREL_LINUX REQUIRED IMPORTED_TARGET
    glib-2.0 gio-2.0 libsecret-1 libpipewire-0.3)

  add_library(quarrel_core STATIC
    ${QUARREL_CORE_SOURCES} ${QUARREL_LINUX_SOURCES})
  target_compile_definitions(quarrel_core PRIVATE _GNU_SOURCE)
  target_link_libraries(quarrel_core PUBLIC
    PkgConfig::QUARREL_LINUX CURL::libcurl z)

  pkg_check_modules(QUARREL_TURBOJPEG IMPORTED_TARGET libturbojpeg)

  if(QUARREL_TURBOJPEG_FOUND)
    target_link_libraries(quarrel_core PUBLIC PkgConfig::QUARREL_TURBOJPEG)
    target_compile_definitions(quarrel_core PRIVATE QUARREL_TURBOJPEG)
  else()
    message(STATUS "quarrel: no libturbojpeg, cameras that film in jpeg are left out")
  endif()
elseif(WIN32)
  find_package(CURL REQUIRED)

  add_library(quarrel_core STATIC
    ${QUARREL_CORE_SOURCES} ${QUARREL_WINDOWS_SOURCES})
  target_compile_definitions(quarrel_core PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX COBJMACROS
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)

  target_compile_definitions(quarrel_core PUBLIC _FILE_OFFSET_BITS=64)
  target_compile_options(quarrel_core PUBLIC
    "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/posix.h>")
  target_link_libraries(quarrel_core PUBLIC
    CURL::libcurl z ws2_32 advapi32 crypt32 bcrypt cfgmgr32 user32 shell32
    ole32 oleaut32 uuid runtimeobject avrt mfplat mf mfreadwrite mfuuid)
else()
  message(FATAL_ERROR "Quarrel runs on macOS, Linux and Windows")
endif()

target_include_directories(quarrel_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quarrel_core PUBLIC
  civue::civue cilo::cilo cima::cima cinc::cinc ciphon::curl ciren::ciren)
target_compile_options(quarrel_core PRIVATE ${QUARREL_C2Y} -Wall -Wextra)

# twemoji's svgs, which discord draws its unicode emoji with, kept in the
# binary: discord.com/assets serves them only while the web client lives.
FetchContent_Declare(twemoji
  URL https://github.com/jdecked/twemoji/archive/refs/tags/v17.0.3.tar.gz
  URL_HASH SHA256=a0855654b633045ae2337537e77f1bb4361162f7fcd910e613eaab1d6d9c5fca)
FetchContent_MakeAvailable(twemoji)

file(GLOB twemoji_svgs RELATIVE ${twemoji_SOURCE_DIR}/assets/svg
  ${twemoji_SOURCE_DIR}/assets/svg/*.svg)
# sorted as strcmp sorts their names, for a bsearch. one after the other,
# the like ones close, they pack to a tenth in a single zstd frame.
list(TRANSFORM twemoji_svgs REPLACE "\\.svg$" "")
list(SORT twemoji_svgs)
set(twemoji_packed ${CMAKE_CURRENT_BINARY_DIR}/twemoji.zst)
if(NOT EXISTS ${twemoji_packed}
    OR NOT twemoji_packed_from STREQUAL twemoji_SOURCE_DIR)
  set(twemoji_all "")
  foreach(name ${twemoji_svgs})
    file(READ ${twemoji_SOURCE_DIR}/assets/svg/${name}.svg svg)
    string(APPEND twemoji_all "${svg}")
  endforeach()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/twemoji.svgs "${twemoji_all}")
  file(ARCHIVE_CREATE OUTPUT ${twemoji_packed}
    PATHS ${CMAKE_CURRENT_BINARY_DIR}/twemoji.svgs
    FORMAT raw COMPRESSION Zstd COMPRESSION_LEVEL 19)
  file(REMOVE ${CMAKE_CURRENT_BINARY_DIR}/twemoji.svgs)
  set(twemoji_packed_from ${twemoji_SOURCE_DIR} CACHE INTERNAL "")
endif()
set(twemoji_rows "")
set(twemoji_at 0)
foreach(name ${twemoji_svgs})
  file(SIZE ${twemoji_SOURCE_DIR}/assets/svg/${name}.svg len)
  string(APPEND twemoji_rows "  {\"${name}\", ${twemoji_at}, ${len}},\n")
  math(EXPR twemoji_at "${twemoji_at} + ${len}")
endforeach()
list(LENGTH twemoji_svgs twemoji_count)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/twemoji.c @ONLY CONTENT
  "#include \"discord/emoji.h\"\n\nstatic const unsigned char packed[] = {\n#embed \"${twemoji_packed}\"\n};\n\nconst unsigned char *const TWEMOJIS_PACKED = packed;\nconst size_t TWEMOJIS_PACKED_LEN = sizeof packed;\nconst size_t TWEMOJIS_LEN = ${twemoji_at};\n\nconst Twemoji TWEMOJIS[] = {\n${twemoji_rows}};\nconst size_t TWEMOJIS_COUNT = ${twemoji_count};\n")
target_sources(quarrel_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/twemoji.c)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
target_link_libraries(quarrel_core PUBLIC PkgConfig::ZSTD)

include(cmake/voice.cmake)
target_link_libraries(quarrel_core PUBLIC
  quarrel_dave quarrel_rnnoise OpenSSL::Crypto PkgConfig::OPUS)

set(HCAPTCHA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(hcaptcha-civue)

add_executable(quarrel
  src/main.c
  src/ui/accounts.c
  src/ui/alerts.c
  src/ui/channels.c
  src/ui/forms.c
  src/ui/inbox.c
  src/ui/messagemenu.c
  src/ui/messagecomponents.c
  src/ui/newdm.c
  src/ui/app.c
  src/ui/chat.c
  src/ui/complete.c
  src/ui/composer.c
  src/ui/cues.c
  src/ui/emojis.c
  src/ui/forum.c
  src/ui/friends.c
  src/ui/gifs.c
  src/ui/guilds.c
  src/ui/apps.c
  src/ui/authorize.c
  src/ui/explore.c
  src/ui/hub.c
  src/ui/login.c
  src/ui/look.c
  src/ui/markup.c
  src/ui/members.c
  src/ui/mentions.c
  src/ui/media.c
  src/ui/messages.c
  src/ui/profile.c
  src/ui/shell.c
  src/ui/roles.c
  src/ui/server.c
  src/ui/settings.c
  src/ui/sidebar.c
  src/ui/stage.c
  src/ui/stickers.c
  src/ui/uploads.c
  src/ui/voice.c
  src/ui/widgets.c)
target_link_libraries(quarrel PRIVATE quarrel_core hcaptcha::civue)
target_compile_options(quarrel PRIVATE ${QUARREL_C2Y} -Wall -Wextra)

civue_icons(quarrel)

if(APPLE)
  set_target_properties(quarrel PROPERTIES
    MACOSX_BUNDLE ON
    OUTPUT_NAME Quarrel
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist
    MACOSX_BUNDLE_GUI_IDENTIFIER com.vexcited.quarrel
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  target_link_options(quarrel PRIVATE LINKER:-dead_strip,-S,-x)

  set(QUARREL_SIGN_IDENTITY "-" CACHE STRING
    "The codesign identity Quarrel is signed with, - for the ad hoc one.")
  if(NOT QUARREL_SIGN_IDENTITY)
    set(QUARREL_SIGN_IDENTITY -)
  endif()

  add_custom_command(TARGET quarrel POST_BUILD
    COMMAND codesign --force --sign "${QUARREL_SIGN_IDENTITY}"
      $<TARGET_BUNDLE_DIR:quarrel>
    VERBATIM)

  install(TARGETS quarrel BUNDLE DESTINATION . COMPONENT quarrel)
  install(CODE "execute_process(COMMAND codesign --force
    --sign [==[${QUARREL_SIGN_IDENTITY}]==]
    \"\${CMAKE_INSTALL_PREFIX}/$<TARGET_BUNDLE_DIR_NAME:quarrel>\"
    COMMAND_ERROR_IS_FATAL ANY)" COMPONENT quarrel)
elseif(WIN32)
  enable_language(RC)
  configure_file(cmake/quarrel.rc ${CMAKE_CURRENT_BINARY_DIR}/quarrel.rc @ONLY)
  target_sources(quarrel PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/quarrel.rc)
  set_target_properties(quarrel PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME Quarrel)
  target_link_options(quarrel PRIVATE LINKER:--gc-sections,-s)

  if(CMAKE_LINKER_TYPE STREQUAL "LLD")
    target_link_options(quarrel PRIVATE LINKER:--icf=safe)
  endif()

  get_filename_component(QUARREL_DLLS ${CMAKE_C_COMPILER} DIRECTORY)
  get_filename_component(QUARREL_MSYS ${QUARREL_DLLS} DIRECTORY)

  if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
  endif()

  install(TARGETS quarrel
    RUNTIME_DEPENDENCIES
      DIRECTORIES ${QUARREL_DLLS}
      PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
      POST_EXCLUDE_REGEXES "[Ss][Yy][Ss][Tt][Ee][Mm]32"
    RUNTIME DESTINATION bin COMPONENT quarrel)

  if(EXISTS ${QUARREL_DLLS}/WebView2Loader.dll)
    install(FILES ${QUARREL_DLLS}/WebView2Loader.dll
      DESTINATION bin COMPONENT quarrel)
  endif()

  if(EXISTS ${QUARREL_MSYS}/etc/ssl/certs/ca-bundle.crt)
    install(FILES ${QUARREL_MSYS}/etc/ssl/certs/ca-bundle.crt
      DESTINATION etc/ssl/certs COMPONENT quarrel)
  endif()
else()
  include(GNUInstallDirs)
  target_link_options(quarrel PRIVATE LINKER:--gc-sections,--as-needed,-s)

  if(CMAKE_LINKER_TYPE STREQUAL "LLD")
    target_link_options(quarrel PRIVATE
      LINKER:--icf=safe,-O2,-z,pack-relative-relocs)
  endif()

  install(TARGETS quarrel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT quarrel)
  install(FILES cmake/quarrel.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT quarrel)
endif()
