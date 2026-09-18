#pragma once

#include "types.h"

void Vellum_VoiceStart();
void Vellum_VoiceStop();
int Vellum_VoiceAvailable(uint32 *pcbCompressed, uint32 *pcbUncompressed, uint32 wantRate);
int Vellum_VoiceGet(bool wantCompressed, void *dst, uint32 dstBytes, uint32 *wrote,
                    bool wantUncompressed, void *udst, uint32 udstBytes, uint32 *uwrote, uint32 wantRate);
int Vellum_VoiceDecompress(const void *comp, uint32 compBytes, void *dst, uint32 dstBytes,
                           uint32 *wrote, uint32 wantRate);
uint32 Vellum_VoiceOptimalRate();
