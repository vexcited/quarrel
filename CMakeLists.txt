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
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "build type" FORCE)
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
  src/json.c
  src/discord/api.c
  src/discord/cdn.c
  src/discord/etf.c
  src/discord/events.c
  src/discord/gateway.c
  src/discord/markdown.c
  src/discord/permissions.c
  src/discord/settings_proto.c
  src/discord/session.c
  src/discord/store.c
  src/discord/util.c)

set(QUARREL_DARWIN_SOURCES
  src/platform/darwin/image.m
  src/platform/darwin/pending.m
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
    "-framework CoreGraphics" "-framework CoreFoundation" z)
else()
  message(FATAL_ERROR "Quarrel only has a macOS backend for now")
endif()

target_include_directories(quarrel_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quarrel_core PUBLIC
  civue::civue cilo::cilo cinc::cinc ciphon::curl)
target_compile_options(quarrel_core PRIVATE -Wall -Wextra)

find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
endif()
if(ZSTD_FOUND)
  target_link_libraries(quarrel_core PUBLIC PkgConfig::ZSTD)
  target_compile_definitions(quarrel_core PRIVATE QUARREL_ZSTD)
endif()

if(NOT TARGET hcaptcha::civue)
  set(HCAPTCHA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(hcaptcha-civue
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../hcaptcha-civue)
  FetchContent_MakeAvailable(hcaptcha-civue)
endif()

add_executable(quarrel
  src/main.c
  src/ui/app.c
  src/ui/chat.c
  src/ui/composer.c
  src/ui/friends.c
  src/ui/guilds.c
  src/ui/login.c
  src/ui/look.c
  src/ui/markup.c
  src/ui/members.c
  src/ui/messages.c
  src/ui/shell.c
  src/ui/sidebar.c
  src/ui/widgets.c)
target_link_libraries(quarrel PRIVATE quarrel_core hcaptcha::civue)
target_compile_options(quarrel PRIVATE -Wall -Wextra)

civue_icons(quarrel)

set(QUARREL_SIGN_IDENTITY "" CACHE STRING
  "The codesign identity Quarrel is signed with, none for the ad hoc one.")

if(APPLE AND QUARREL_SIGN_IDENTITY)
  add_custom_command(TARGET quarrel POST_BUILD
    COMMAND codesign --force --sign "${QUARREL_SIGN_IDENTITY}"
      --identifier com.vexcited.quarrel $<TARGET_FILE:quarrel>
    VERBATIM)
endif()
