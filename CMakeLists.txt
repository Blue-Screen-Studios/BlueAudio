cmake_minimum_required(VERSION 3.21)
project(blueaudio VERSION 0.1.0 LANGUAGES C)

# BlueAudio: the sound of a world, traced, in C. Sources (the game's
# waveforms) and voxels in; rays fired from the listener; the final mix out,
# a waveform for each channel. No sound of its own, no audio device. Built
# alone (its demo, its tests) or inside a game (add_subdirectory /
# FetchContent: the library).
set(BLUEAUDIO_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})
option(BLUEAUDIO_SHARED "A shared library (libblueaudio.so), linked by the game at run time" ON)
option(BLUEAUDIO_BUILD_DEMO "The demo (SDL3: a window, its audio)" ${BLUEAUDIO_TOP_LEVEL})
option(BLUEAUDIO_BUILD_TESTS "The tests (ctest)" ${BLUEAUDIO_TOP_LEVEL})

if(BLUEAUDIO_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(BLUEAUDIO_SHARED)
  set(BLUEAUDIO_LINKAGE SHARED)
else()
  set(BLUEAUDIO_LINKAGE STATIC)
endif()
find_package(Threads REQUIRED)

# ------------------------------------------------------------ the library
add_library(blueaudio ${BLUEAUDIO_LINKAGE} src/context.c src/world.c src/trace.c src/render.c src/reverb.c src/paths.c)
target_include_directories(blueaudio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(blueaudio PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON
  C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_definitions(blueaudio PRIVATE BLUEAUDIO_BUILDING)
if(BLUEAUDIO_SHARED)
  target_compile_definitions(blueaudio INTERFACE BLUEAUDIO_DLL)
endif()
target_link_libraries(blueaudio PRIVATE Threads::Threads)
if(UNIX)
  target_link_libraries(blueaudio PRIVATE m)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(blueaudio PRIVATE -Wall -Wextra)
  # (Its rays always optimized: a game's Debug build still traces in time.)
  target_compile_options(blueaudio PRIVATE $<$<OR:$<CONFIG:Debug>,$<CONFIG:>>:-O2>)
endif()

# ------------------------------------------------------------ the demo, the tests
if(BLUEAUDIO_BUILD_DEMO)
  find_package(SDL3 CONFIG QUIET)
  if(TARGET SDL3::SDL3)
    set(BLUEAUDIO_DEMO_SDL3 SDL3::SDL3)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BLUEAUDIO_DEMO_SDL3_PC REQUIRED IMPORTED_TARGET sdl3)
    set(BLUEAUDIO_DEMO_SDL3 PkgConfig::BLUEAUDIO_DEMO_SDL3_PC)
  endif()
  # BlueUI: the demo's panel and its narrator. The repository beside this
  # one if it's there (https://github.com/Blue-Screen-Studios/BlueUI), else
  # fetched.
  if(NOT TARGET blueui)
    set(BLUEUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../BlueUI" CACHE PATH "BlueUI's source (its repository)")
    set(BLUEUI_BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(BLUEUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(EXISTS "${BLUEUI_DIR}/CMakeLists.txt")
      add_subdirectory("${BLUEUI_DIR}" blueui)
    else()
      include(FetchContent)
      FetchContent_Declare(blueaudio_blueui GIT_REPOSITORY https://github.com/Blue-Screen-Studios/BlueUI.git GIT_TAG main)
      FetchContent_MakeAvailable(blueaudio_blueui)
    endif()
  endif()
  # dr_mp3 (public domain: David Reid's dr_libs), the music decoded.
  include(FetchContent)
  FetchContent_Declare(
    blueaudio_dr_mp3
    URL      https://raw.githubusercontent.com/mackron/dr_libs/51e61d308dde6b437fce0c5fabb32cd86b40f4d7/dr_mp3.h
    URL_HASH SHA256=997b7ee18de6e6b81e2a83f1ea9fc62aef25c62b28d48db95635f49e65de0a2f
    DOWNLOAD_NO_EXTRACT TRUE
  )
  FetchContent_MakeAvailable(blueaudio_dr_mp3)
  # The music (Joystock's), compiled in.
  add_executable(blueaudio_embed demo/embed.c)
  set(BLUEAUDIO_MUSIC "${CMAKE_CURRENT_SOURCE_DIR}/demo/music/joystock-corporate-pop.mp3")
  set(BLUEAUDIO_MUSIC_C "${CMAKE_CURRENT_BINARY_DIR}/demo_music.c")
  add_custom_command(
    OUTPUT "${BLUEAUDIO_MUSIC_C}"
    COMMAND blueaudio_embed "${BLUEAUDIO_MUSIC}" "${BLUEAUDIO_MUSIC_C}" demo_music
    DEPENDS blueaudio_embed "${BLUEAUDIO_MUSIC}"
    VERBATIM)
  add_executable(blueaudio_demo demo/main.c "${BLUEAUDIO_MUSIC_C}")
  set_target_properties(blueaudio_demo PROPERTIES C_STANDARD 11)
  target_include_directories(blueaudio_demo SYSTEM PRIVATE "${blueaudio_dr_mp3_SOURCE_DIR}")
  target_link_libraries(blueaudio_demo PRIVATE blueaudio blueui ${BLUEAUDIO_DEMO_SDL3} m)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(blueaudio_demo PRIVATE -Wall -Wextra)
  endif()
endif()

if(BLUEAUDIO_BUILD_TESTS)
  enable_testing()
  add_executable(blueaudio_tests tests/tests.c)
  set_target_properties(blueaudio_tests PROPERTIES C_STANDARD 11)
  target_link_libraries(blueaudio_tests PRIVATE blueaudio m)
  add_test(NAME blueaudio_tests COMMAND blueaudio_tests)
endif()
