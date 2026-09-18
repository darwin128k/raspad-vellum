if(WIN32)
    add_library(steamclient SHARED
        dllmain.cpp
        identity.cpp
        steam_ifaces.cpp
        steam_query.cpp
        steamclient.def
    )
    target_link_libraries(steamclient PRIVATE advapi32 ws2_32)
    target_compile_definitions(steamclient PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    add_library(steamclient SHARED
        identity.cpp
        steam_ifaces.cpp
        steam_query.cpp
    )
    target_compile_options(steamclient PRIVATE -fvisibility=hidden)
    target_link_options(steamclient PRIVATE -static-libstdc++ -static-libgcc)
endif()

target_include_directories(steamclient PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if (MSVC)
    target_compile_options(steamclient PRIVATE /utf-8)
endif()

set_target_properties(steamclient PROPERTIES
    OUTPUT_NAME steamclient
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
