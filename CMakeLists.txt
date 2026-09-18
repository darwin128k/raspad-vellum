cmake_minimum_required(VERSION 3.16)
project(vellum LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(WIN32)
    enable_language(RC)
elseif(UNIX AND NOT APPLE)
    # GoldSrc client is 32-bit. Match steam_api.so / hw.so and raspad-hl.
    message(STATUS "vellum: targeting 32-bit Linux (hw.so)")
    add_compile_options(-m32 -Wno-format-truncation)
    add_link_options(-m32)
else()
    message(FATAL_ERROR "Vellum supports Windows and Linux")
endif()

option(REVLOADER_STANDALONE "Run GoldSrc in this process (compile hl launcher sources from raspad-hl) instead of spawning hl" OFF)
option(HL_METAHHOOK "Embed MetaHookSv in the standalone launcher" OFF)
option(VELLUM_NO_SERVER_BROWSER "Internet tab stays empty (no baked master)" OFF)
set(HL_DIR "" CACHE PATH "Path to raspad-hl (required when REVLOADER_STANDALONE=ON)")
set(METAHOOK_DIR "" CACHE PATH "Path to MetaHookSv (default: HL_DIR/../metahook)")

if(HL_METAHHOOK AND NOT WIN32)
    message(FATAL_ERROR "HL_METAHHOOK is Windows-only")
endif()

add_subdirectory(src/loader)
add_subdirectory(src/steamclient)
