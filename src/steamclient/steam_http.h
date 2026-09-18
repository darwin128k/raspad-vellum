#pragma once

#include "types.h"

void *Vellum_SteamHTTP();
void Vellum_HttpThink();
bool Vellum_HttpIsCallCompleted(SteamAPICall_t call, bool *failed);
bool Vellum_HttpCallPending(SteamAPICall_t call);
bool Vellum_HttpGetCallResult(SteamAPICall_t call, void *data, int cub, int expected, bool *failed);
#ifndef _WIN32
int Vellum_NetHttpGet(const char *url, const char *ua, int timeout_sec, int head_only,
                      uint8 **out_body, uint32 *out_n, uint32 *out_status, volatile int *abort_flag);
#endif
void Vellum_QueueCallback(HSteamUser user, int id, const void *data, int size);
void Vellum_Log(const char *fmt, ...);
