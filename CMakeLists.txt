cmake_minimum_required(VERSION 3.16)
project(vellum LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

enable_language(RC)

option(REVLOADER_STANDALONE "Run GoldSrc in this process (compile hl launcher sources) instead of spawning hl.exe" OFF)
option(REVLOADER_LAUNCHER_DLLS "Forward rev.ini [Loader] Dlls= as -dll flags" ON)
option(HL_METAHHOOK "Embed MetaHookSv in the standalone launcher" OFF)
set(HL_DIR "" CACHE PATH "Path to the hl launcher repository (required when REVLOADER_STANDALONE=ON)")
set(METAHOOK_DIR "" CACHE PATH "Path to MetaHookSv (default: HL_DIR/../metahook)")

add_subdirectory(src/loader)
add_subdirectory(src/steamclient)
