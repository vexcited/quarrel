cmake_minimum_required(VERSION 3.29)
project(quarrel VERSION 0.0.0 LANGUAGES C OBJC)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_OBJC_STANDARD 23)
set(CMAKE_OBJC_STANDARD_REQUIRED ON)
set(CMAKE_OBJC_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES
  AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

if(PROJECT_IS_TOP_LEVEL AND APPLE
  AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX /Applications CACHE PATH "install prefix" FORCE)
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
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../ciko-cilo/CMakeLists.txt)
  set(FETCHCONTENT_SOURCE_DIR_CILO ${CMAKE_CURRENT_SOURCE_DIR}/../ciko-cilo
    CACHE PATH "the cilo checkout")
endif()
if(NOT TARGET civue::civue)
  set(CIVUE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(civue
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ciko-civue)
  FetchContent_MakeAvailable(civue)
endif()
if(NOT TARGET cinc::cinc)
  FetchContent_Declare(cinc
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ciko-cinc)
  FetchContent_MakeAvailable(cinc)
endif()
if(NOT TARGET cima::cima)
  FetchContent_Declare(cima
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ciko-cima)
  FetchContent_MakeAvailable(cima)
endif()
if(NOT TARGET ciphon::curl)
  set(CIPHON_BINDINGS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(ciphon
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ciko-cilent)
  FetchContent_MakeAvailable(ciphon)
endif()
if(NOT TARGET ciphon::curl)
  message(FATAL_ERROR "Quarrel needs libcurl ")
endif()

set(QUARREL_CORE_SOURCES
  src/http.c
  src/images.c
  src/discord/api.c
  src/discord/cdn.c
  src/discord/emoji.c
  src/discord/emoji_table.c
  src/discord/etf.c
  src/discord/events.c
  src/discord/gateway.c
  src/discord/gifs.c
  src/discord/markdown.c
  src/discord/permissions.c
  src/discord/remote_auth.c
  src/discord/settings_proto.c
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
  src/platform/darwin/pending.m
  src/platform/darwin/qr.m
  src/platform/darwin/storage.m
  src/platform/darwin/system.m
  src/platform/darwin/timer.m
  src/platform/darwin/websocket.m)

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
    "-framework Metal" "-framework AppKit" "-framework Network" z)
else()
  message(FATAL_ERROR "Quarrel only has a macOS backend for now")
endif()

target_include_directories(quarrel_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quarrel_core PUBLIC
  civue::civue cilo::cilo cima::cima cinc::cinc ciphon::curl)
target_compile_options(quarrel_core PRIVATE -Wall -Wextra)

find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
endif()
if(ZSTD_FOUND)
  target_link_libraries(quarrel_core PUBLIC PkgConfig::ZSTD)
  target_compile_definitions(quarrel_core PRIVATE QUARREL_ZSTD)
endif()

include(cmake/voice.cmake)
target_link_libraries(quarrel_core PUBLIC
  quarrel_dave quarrel_rnnoise OpenSSL::Crypto PkgConfig::OPUS)

if(NOT TARGET hcaptcha::civue)
  set(HCAPTCHA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(hcaptcha-civue
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../hcaptcha-civue)
  FetchContent_MakeAvailable(hcaptcha-civue)
endif()

add_executable(quarrel
  src/main.c
  src/ui/accounts.c
  src/ui/inbox.c
  src/ui/messagemenu.c
  src/ui/newdm.c
  src/ui/app.c
  src/ui/chat.c
  src/ui/composer.c
  src/ui/forum.c
  src/ui/friends.c
  src/ui/gifs.c
  src/ui/guilds.c
  src/ui/login.c
  src/ui/look.c
  src/ui/markup.c
  src/ui/members.c
  src/ui/media.c
  src/ui/messages.c
  src/ui/profile.c
  src/ui/shell.c
  src/ui/settings.c
  src/ui/sidebar.c
  src/ui/stage.c
  src/ui/uploads.c
  src/ui/voice.c
  src/ui/widgets.c)
target_link_libraries(quarrel PRIVATE quarrel_core hcaptcha::civue)
target_compile_options(quarrel PRIVATE -Wall -Wextra)

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
endif()
