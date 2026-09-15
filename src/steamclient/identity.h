#pragma once

#include "types.h"
#include "vellum_ticket.h"

struct VellumIdentity {
	char persona[64];
	char ident[VELLUM_IDENT_MAX];
	uint16 ident_len;
	uint32 account_id;
	uint32 app_id;
	CSteamID steam_id;
};

void Vellum_InitIdentity();
const VellumIdentity &Vellum_GetIdentity();
int Vellum_WriteAuthBlob(void *blob, int maxBytes);
