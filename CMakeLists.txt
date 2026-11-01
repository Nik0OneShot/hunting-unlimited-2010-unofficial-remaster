cmake_minimum_required(VERSION 3.24)
project(longshot LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The game is 32-bit, so Longshot must be too. Configure with: cmake -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(OUT ${CMAKE_SOURCE_DIR}/dist)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUT})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${OUT})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${OUT})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${OUT})

add_compile_options(/W4 /utf-8 /permissive- /EHsc)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
# The launcher stub is located by address, so incremental-link thunks must not exist.
add_link_options(/INCREMENTAL:NO)

add_library(longshot_core SHARED
    src/core/config.cpp
    src/core/core.cpp
    src/core/crash.cpp
    src/core/display.cpp
    src/core/input.cpp
    src/core/layout.cpp
    src/core/log.cpp
    src/core/menu.cpp
    src/core/patch.cpp
    src/core/settings.cpp
    src/core/sound.cpp
    src/core/timing.cpp
    src/core/view.cpp
)
target_compile_definitions(longshot_core PRIVATE DIRECTINPUT_VERSION=0x0800)
target_link_libraries(longshot_core PRIVATE user32 gdi32 winmm)

add_executable(longshot WIN32 src/launcher/main.cpp)
target_link_libraries(longshot PRIVATE user32 advapi32 shell32)

# Stand-in for the game, so injection can be tested without launching it.
add_executable(longshot_selftest tests/selftest.cpp)
target_link_libraries(longshot_selftest PRIVATE user32)

add_custom_command(TARGET longshot POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/longshot.default.ini ${OUT}/longshot.default.ini)
