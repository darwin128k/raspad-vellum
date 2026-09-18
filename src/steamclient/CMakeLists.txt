if(WIN32)
    include(FetchContent)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
    FetchContent_Declare(opus
        GIT_REPOSITORY https://github.com/xiph/opus.git
        GIT_TAG v1.5.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(opus)

    add_library(steamclient SHARED
        dllmain.cpp
        identity.cpp
        steam_ifaces.cpp
        steam_query.cpp
        steam_http.cpp
        steam_voice.cpp
        steamclient.def
    )
    target_link_libraries(steamclient PRIVATE advapi32 ws2_32 wininet winmm opus)
    target_compile_definitions(steamclient PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    add_library(steamclient SHARED
        identity.cpp
        steam_ifaces.cpp
        steam_query.cpp
        steam_http.cpp
        steam_voice.cpp
    )
    target_compile_options(steamclient PRIVATE -fvisibility=hidden)
    target_link_options(steamclient PRIVATE -static-libstdc++ -static-libgcc)
    target_link_libraries(steamclient PRIVATE pthread dl opus asound)
endif()

target_include_directories(steamclient PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if (MSVC)
    target_compile_options(steamclient PRIVATE /utf-8)
endif()

if(VELLUM_NO_SERVER_BROWSER)
    target_compile_definitions(steamclient PRIVATE VELLUM_NO_SERVER_BROWSER)
    message(STATUS "vellum: Internet server browser disabled (no master.vdf)")
endif()

set_target_properties(steamclient PROPERTIES
    OUTPUT_NAME steamclient
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
