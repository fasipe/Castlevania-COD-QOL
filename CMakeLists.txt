cmake_minimum_required(VERSION 3.24)
project(codport LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  add_link_options($<$<CONFIG:Release>:/LTCG:INCREMENTAL>)
endif()

set(SOURCES
  src/main.cpp
  src/iso9660.cpp
  src/elf.cpp
  src/ee/memory.cpp
  src/ee/hardware.cpp
  src/ee/cpu.cpp
  src/ee/vu.cpp
  src/ee/kernel.cpp
  src/gs/dmac.cpp
  src/gs/gif.cpp
  src/gs/gs.cpp
  src/gs/gsmem.cpp
  src/gs/raster.cpp
  src/gs/graphics.cpp
  src/gs/gpu.cpp
  src/gs/frame_gen.cpp
  src/gs/shader_cache.cpp
  src/gs/texture_pack.cpp
  src/gs/vif.cpp
  src/ipu/ipu.cpp
  src/host/window.cpp
  src/recomp/generator.cpp
  src/iop/sif.cpp
  src/iop/cdvd.cpp
  src/iop/fileio.cpp
  src/iop/modules.cpp
  src/iop/sound.cpp
  src/iop/memcard.cpp
  src/iop/pad.cpp
  src/iop/iopcpu.cpp
  src/iop/iopkernel.cpp
  src/iop/spu2.cpp
  src/host/sampler.cpp
  src/host/audio.cpp
  src/host/gamepad.cpp
  src/host/settings.cpp
  src/host/controls_dialog.cpp
  src/host/icon.cpp
  src/host/screenshot.cpp
  src/game/attack_turn.cpp
  src/game/minimap.cpp
  src/game/mouse_camera.cpp
  src/game/sprint.cpp
  src/game/widescreen.cpp
  src/recomp/vu_generator.cpp
)
option(CODPORT_GAME_ONLY "Build only codport_game.dll, next to a codport.exe and codport.lib already built" OFF)
if(NOT CODPORT_GAME_ONLY)
# Built for any x64 processor (no /arch:AVX2: the same speed, measured; the CPU renderer's AVX2 code
# runs only where the processor has it, and without it the graphics card must draw).
add_executable(codport ${SOURCES} src/recomp/gamecode.cpp tools/setup/icon.rc)
if(MSVC)
  # Debug information for the hand-written code (not the generated code: too large), so that
  # the sampling profiler (CODPORT_SAMPLE) can name source lines.
  # Whole-program optimization for these only: the interpreters call small functions across files.
  # Over the generated code it made the final link take most of the build (minutes, mostly on one core).
  set_source_files_properties(${SOURCES} PROPERTIES COMPILE_OPTIONS "/Zi;$<$<CONFIG:Release>:/GL>")
  target_link_options(codport PRIVATE /DEBUG /OPT:REF /OPT:ICF)
  # A Windows program, so opening it shows no console window; main() still starts it, and its output
  # goes to the console it was started from (useParentConsole) or wherever it is redirected.
  target_link_options(codport PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

target_include_directories(codport PRIVATE src)
target_link_libraries(codport PRIVATE user32 gdi32 comctl32 comdlg32 dbghelp winmm xinput d3d11 dxgi d3dcompiler ole32 windowscodecs)
# The game's code: codport_game.dll, built from the player's own disc (the recompiled functions in
# recomp/gen and the VU micro programs in recomp/vu, written by `codport --recompile` and
# `--vu-recompile`, see recompile.bat), loaded at start (src/recomp/gamecode.cpp). codport.exe
# exports what it uses (CODPORT_API) and its import library, codport.lib, is what the DLL links.
set_target_properties(codport PROPERTIES ENABLE_EXPORTS ON)

# Setup.exe: opens the setup window (tools/setup/setup.ps1) as Setup.bat does, with an icon.
add_executable(Setup WIN32 tools/setup/launcher.cpp tools/setup/icon.rc)
# Texture Pack Mod.exe: the same for the texture pack installer (tools/setup/texture-pack-mod.ps1).
add_executable(TexturePackMod WIN32 tools/setup/launcher.cpp tools/setup/icon.rc)
target_compile_definitions(TexturePackMod PRIVATE TEXTURE_PACK_MOD)
set_target_properties(TexturePackMod PROPERTIES OUTPUT_NAME "Texture Pack Mod")
endif()

# What the game runs from its start (recomp/gen/boot), built first so the game can start; then the
# VU programs (recomp/vu), the rest of what it runs (recomp/gen/hot) and everything else
# (recomp/gen/cold), each in a DLL of its own that the game loads once it is built (see Setup.bat).
if(EXISTS ${CMAKE_SOURCE_DIR}/recomp/gen/boot/table.cpp)
  file(GLOB BOOT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/recomp/gen/boot/*.cpp)
  file(GLOB HOT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/recomp/gen/hot/*.cpp)
  file(GLOB COLD_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/recomp/gen/cold/*.cpp)
  file(GLOB VU_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/recomp/vu/*.cpp)
  add_library(codport_game SHARED ${BOOT_SOURCES})
  add_library(codport_game_vu SHARED ${VU_SOURCES})
  add_library(codport_game_hot SHARED ${HOT_SOURCES})
  add_library(codport_game_cold SHARED ${COLD_SOURCES})
  foreach(game codport_game codport_game_vu codport_game_hot codport_game_cold)
    target_compile_definitions(${game} PRIVATE CODPORT_GAME_DLL)
    # Every generated file includes the same headers: parsed once.
    target_precompile_headers(${game} PRIVATE src/recomp/runtime.h src/ee/vu_ops.h)
    target_include_directories(${game} PRIVATE src)
    if(CODPORT_GAME_ONLY)  # codport.exe and codport.lib already in build/ (a release): only the game's code
      set_target_properties(${game} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
      target_link_libraries(${game} PRIVATE ${CMAKE_SOURCE_DIR}/build/codport.lib)
    else()
      set_target_properties(${game} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
      target_link_libraries(${game} PRIVATE codport)
    endif()
  endforeach()
  if(MSVC)
    set_source_files_properties(${BOOT_SOURCES} ${HOT_SOURCES} ${COLD_SOURCES} ${VU_SOURCES} PROPERTIES COMPILE_OPTIONS "/bigobj;/wd4102;/wd4065")
  endif()
endif()
