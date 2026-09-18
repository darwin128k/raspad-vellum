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

typedef void (*VellumMasterCb)(void *user, uint32 ip, uint16 port, int finished);
int Vellum_MasterAdd(const char *address, const char *filter, VellumMasterCb cb, void *user);
int Vellum_LanStart(VellumMasterCb cb, void *user);
void Vellum_MasterCancel(void *user);
