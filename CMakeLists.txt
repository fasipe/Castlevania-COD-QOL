cmake_minimum_required(VERSION 3.24)
project(codport LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  # Whole-program optimization: the interpreters call small functions across files.
  add_compile_options($<$<CONFIG:Release>:/GL> /arch:AVX2)
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
  src/game/attack_turn.cpp
  src/game/minimap.cpp
  src/game/sprint.cpp
  src/game/widescreen.cpp
  src/recomp/vu_generator.cpp
)
add_executable(codport ${SOURCES})
if(MSVC)
  # Debug information for the hand-written code (not the generated code: too large), so that
  # the sampling profiler (CODPORT_SAMPLE) can name source lines.
  set_source_files_properties(${SOURCES} PROPERTIES COMPILE_OPTIONS "/Zi")
  target_link_options(codport PRIVATE /DEBUG /OPT:REF /OPT:ICF)
  # A Windows program, so opening it shows no console window; main() still starts it, and its output
  # goes to the console it was started from (useParentConsole) or wherever it is redirected.
  target_link_options(codport PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

# Recompiled game code (recomp/gen, written by `codport --recompile`, see recompile.bat).
if(EXISTS ${CMAKE_SOURCE_DIR}/recomp/gen/table.cpp)
  file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/recomp/gen/*.cpp)
  target_sources(codport PRIVATE ${RECOMP_SOURCES})
  if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "/bigobj;/wd4102;/wd4065")
  endif()
else()
  target_sources(codport PRIVATE src/recomp/none.cpp)
endif()

target_include_directories(codport PRIVATE src)
target_link_libraries(codport PRIVATE user32 gdi32 comctl32 comdlg32 dbghelp winmm xinput d3d11 dxgi d3dcompiler)

# Compiled VU micro programs (recomp/vu, written by `codport --vu-recompile`, see recompile.bat).
if(EXISTS ${CMAKE_SOURCE_DIR}/recomp/vu/table.cpp)
  file(GLOB VU_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/recomp/vu/*.cpp)
  target_sources(codport PRIVATE ${VU_SOURCES})
  if(MSVC)
    set_source_files_properties(${VU_SOURCES} PROPERTIES COMPILE_OPTIONS "/bigobj;/Zi")  # small: profilable
  endif()
else()
  target_sources(codport PRIVATE src/recomp/vunone.cpp)
endif()
