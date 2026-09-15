#pragma once

#ifdef _WIN32
#define STEAM_CALL __cdecl
#define STEAM_EXPORT extern "C" __declspec(dllexport)
#else
#define STEAM_CALL
#define STEAM_EXPORT extern "C" __attribute__((visibility("default")))
#endif
