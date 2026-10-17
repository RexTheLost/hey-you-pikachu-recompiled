cmake_minimum_required(VERSION 3.20)

# Hey You, Pikachu! (USA) — recompiled PC build.
# Structure follows Zelda64Recomp (GPL-3.0): N64ModernRuntime + RT64 + SDL2.
# Windows: configure from a VS 2022 x64 dev environment with clang-cl (see tools/build.bat).

project(HeyYouPikachuRecompiled LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything /W4")
endif()

if (WIN32)
    # Opt out of the constexpr mutex constructor to avoid vcredist mismatches.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
    add_compile_definitions(NOMINMAX)
endif()

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)
target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

# RecompiledFuncs — N64Recomp output (regenerate with tools/regen.sh)
add_library(RecompiledFuncs STATIC)
target_compile_options(RecompiledFuncs PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)
if (MSVC)
    target_compile_options(RecompiledFuncs PRIVATE /FI${CMAKE_SOURCE_DIR}/include/hyp_extra_decls.h)
else()
    target_compile_options(RecompiledFuncs PRIVATE -include ${CMAKE_SOURCE_DIR}/include/hyp_extra_decls.h)
endif()
target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)
file(GLOB FUNC_C_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})

add_executable(HeyYouPikachuRecompiled)
target_sources(HeyYouPikachuRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/rt64_render_context.cpp
    ${CMAKE_SOURCE_DIR}/src/game/input.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime_gaps.cpp
    ${CMAKE_SOURCE_DIR}/src/debug_trace.cpp
    ${CMAKE_SOURCE_DIR}/src/overlay_hooks.cpp
    ${CMAKE_SOURCE_DIR}/src/vru/vru.cpp
    ${CMAKE_SOURCE_DIR}/src/vru/vru_picker.cpp
    ${CMAKE_SOURCE_DIR}/rsp/n_aspMain.cpp
)
target_compile_definitions(HeyYouPikachuRecompiled PRIVATE HYP_ROM_XXH3=0x9D29140FAB450973ULL)
target_include_directories(HeyYouPikachuRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/render
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_BINARY_DIR}/rt64/src
)
target_compile_options(HeyYouPikachuRecompiled PRIVATE -fno-strict-aliasing -fms-extensions)
if (CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_compile_options(HeyYouPikachuRecompiled PRIVATE -march=nehalem)
endif()

if (WIN32)
    include(FetchContent)
    set(SDL2_VERSION "2.30.3")
    FetchContent_Declare(
        sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)
    target_include_directories(HeyYouPikachuRecompiled PRIVATE ${sdl2_SOURCE_DIR}/include)
    target_link_directories(HeyYouPikachuRecompiled PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)
    target_link_libraries(HeyYouPikachuRecompiled PRIVATE SDL2 Winmm.lib)

    add_custom_command(TARGET HeyYouPikachuRecompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:HeyYouPikachuRecompiled>)

    # Console window in all configs for now: stdout carries RT64/runtime diagnostics.
    set_target_properties(HeyYouPikachuRecompiled PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

target_link_libraries(HeyYouPikachuRecompiled PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
)
