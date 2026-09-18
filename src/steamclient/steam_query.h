#pragma once

#include "types.h"

void Vellum_QueryInit();
void Vellum_QueryThink();
HServerQuery Vellum_QueryPing(uint32 ip, uint16 port, void *cb);
HServerQuery Vellum_QueryPlayers(uint32 ip, uint16 port, void *cb);
HServerQuery Vellum_QueryRules(uint32 ip, uint16 port, void *cb);
HServerQuery Vellum_QueryInfo(uint32 ip, uint16 port, void (*cb)(void *, int, const VellumGameServerItem *), void *user);
void Vellum_QueryCancel(HServerQuery q);
int Vellum_QueryBusyCount();
