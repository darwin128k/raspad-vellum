#include "steam_query.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#define VELLUM_QUERY_MAX 16
#define VELLUM_QUERY_TIMEOUT 3000

enum {
	VELLUM_Q_PING = 1,
	VELLUM_Q_PLAYERS = 2,
	VELLUM_Q_RULES = 3
};

class SteamPingResponse {
public:
	virtual void ServerResponded(VellumGameServerItem &) = 0;
	virtual void ServerFailedToRespond() = 0;
};

class SteamPlayersResponse {
public:
	virtual void AddPlayerToList(const char *, int, float) = 0;
	virtual void PlayersFailedToRespond() = 0;
	virtual void PlayersRefreshComplete() = 0;
};

class SteamRulesResponse {
public:
	virtual void RulesResponded(const char *, const char *) = 0;
	virtual void RulesFailedToRespond() = 0;
	virtual void RulesRefreshComplete() = 0;
};

struct VellumQuery {
	int used;
	int type;
	HServerQuery id;
	uint32 ip;
	uint16 port;
	void *cb;
	void (*info_cb)(void *, int, const VellumGameServerItem *);
	void *info_user;
	SOCKET sock;
	unsigned sent_ms;
	int challenged;
};

static VellumQuery g_queries[VELLUM_QUERY_MAX];
static HServerQuery g_query_next = 1;
static int g_query_wsa;

static unsigned Vellum_NowMs()
{
#ifdef _WIN32
	return GetTickCount();
#else
	return (unsigned)(clock() * 1000 / CLOCKS_PER_SEC);
#endif
}

void Vellum_QueryInit()
{
#ifdef _WIN32
	WSADATA wsa;
	if (!g_query_wsa) {
		if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
			g_query_wsa = 1;
		}
	}
#else
	g_query_wsa = 1;
#endif
}

static void Vellum_SetNonblock(SOCKET s)
{
#ifdef _WIN32
	u_long one = 1;
	ioctlsocket(s, FIONBIO, &one);
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(s, F_SETFL, flags | O_NONBLOCK);
	}
#endif
}

static SOCKET Vellum_OpenUdp()
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		return INVALID_SOCKET;
	}
	Vellum_SetNonblock(s);
	return s;
}

static int Vellum_SendTo(SOCKET s, uint32 ip, uint16 port, const void *data, int n)
{
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(ip);
	return sendto(s, (const char *)data, n, 0, (struct sockaddr *)&a, sizeof(a));
}

static int Vellum_RecvFrom(SOCKET s, void *data, int n)
{
	struct sockaddr_in a;
#ifdef _WIN32
	int alen = (int)sizeof(a);
#else
	socklen_t alen = sizeof(a);
#endif
	return recvfrom(s, (char *)data, n, 0, (struct sockaddr *)&a, &alen);
}

static int Vellum_ReadStr(const uint8 *p, int n, int *off, char *out, int outsz)
{
	int i = *off;
	int k = 0;
	if (outsz <= 0) {
		return 0;
	}
	out[0] = '\0';
	while (i < n && p[i] != 0 && k < outsz - 1) {
		out[k++] = (char)p[i++];
	}
	out[k] = '\0';
	if (i < n && p[i] == 0) {
		i++;
	}
	*off = i;
	return 1;
}

static void Vellum_SendInfo(VellumQuery *q, uint32 challenge, int has_ch)
{
	uint8 pkt[64];
	int n = 0;
	memset(pkt, 0xFF, 4);
	n = 4;
	pkt[n++] = 'T';
	memcpy(pkt + n, "Source Engine Query", 19);
	n += 19;
	pkt[n++] = 0;
	if (has_ch) {
		memcpy(pkt + n, &challenge, 4);
		n += 4;
	}
	Vellum_SendTo(q->sock, q->ip, q->port, pkt, n);
	{
		uint8 oldq[12];
		memset(oldq, 0xFF, 4);
		memcpy(oldq + 4, "details", 7);
		oldq[11] = 0;
		Vellum_SendTo(q->sock, q->ip, q->port, oldq, 12);
	}
}

static void Vellum_SendPlayers(VellumQuery *q, uint32 challenge)
{
	uint8 pkt[9];
	memset(pkt, 0xFF, 4);
	pkt[4] = 'U';
	memcpy(pkt + 5, &challenge, 4);
	Vellum_SendTo(q->sock, q->ip, q->port, pkt, 9);
}

static void Vellum_SendRules(VellumQuery *q, uint32 challenge)
{
	uint8 pkt[9];
	memset(pkt, 0xFF, 4);
	pkt[4] = 'V';
	memcpy(pkt + 5, &challenge, 4);
	Vellum_SendTo(q->sock, q->ip, q->port, pkt, 9);
}

static VellumQuery *Vellum_QueryAlloc(int type, uint32 ip, uint16 port, void *cb)
{
	int i;
	VellumQuery *q = NULL;
	Vellum_QueryInit();
	if (!g_query_wsa) {
		return NULL;
	}
	for (i = 0; i < VELLUM_QUERY_MAX; i++) {
		if (!g_queries[i].used) {
			q = &g_queries[i];
			break;
		}
	}
	if (q == NULL) {
		return NULL;
	}
	memset(q, 0, sizeof(*q));
	q->sock = Vellum_OpenUdp();
	if (q->sock == INVALID_SOCKET) {
		return NULL;
	}
	q->used = 1;
	q->type = type;
	q->id = g_query_next++;
	if (g_query_next <= 0) {
		g_query_next = 1;
	}
	q->ip = ip;
	q->port = port;
	q->cb = cb;
	q->sent_ms = Vellum_NowMs();
	return q;
}

static void Vellum_QueryClose(VellumQuery *q)
{
	if (q->sock != INVALID_SOCKET) {
		closesocket(q->sock);
		q->sock = INVALID_SOCKET;
	}
	q->used = 0;
	q->cb = NULL;
}

static void Vellum_FillBase(VellumGameServerItem *it, uint32 ip, uint16 port)
{
	memset(it, 0, sizeof(*it));
	it->adr.conn = port;
	it->adr.query = port;
	it->adr.ip = ip;
	it->hadResponse = true;
	it->appId = 10;
}

static int Vellum_ParseInfo(const uint8 *p, int n, VellumGameServerItem *it)
{
	int off;
	if (n < 6 || p[0] != 0xFF || p[1] != 0xFF || p[2] != 0xFF || p[3] != 0xFF) {
		return 0;
	}
	off = 5;
	if (p[4] == 'm') {
		char addr[64];
		Vellum_ReadStr(p, n, &off, addr, sizeof(addr));
		Vellum_ReadStr(p, n, &off, it->name, sizeof(it->name));
		Vellum_ReadStr(p, n, &off, it->map, sizeof(it->map));
		Vellum_ReadStr(p, n, &off, it->gameDir, sizeof(it->gameDir));
		Vellum_ReadStr(p, n, &off, it->desc, sizeof(it->desc));
		if (off + 7 <= n) {
			it->players = p[off++];
			it->maxPlayers = p[off++];
			it->version = p[off++];
			off++; /* type */
			off++; /* os */
			it->password = p[off++] != 0;
			if (p[off++] != 0) {
				int link = 0, url = 0, dl = 0;
				if (off + 2 <= n) {
					link = p[off] | (p[off + 1] << 8);
					off += 2;
				}
				Vellum_ReadStr(p, n, &off, addr, sizeof(addr));
				Vellum_ReadStr(p, n, &off, addr, sizeof(addr));
				if (off + 16 <= n) {
					off += 10;
					url = p[off++];
					dl = p[off++];
					off += 9;
				}
				(void)link;
				(void)url;
				(void)dl;
			}
			if (off < n) {
				it->secure = p[off++] != 0;
			}
			if (off < n) {
				it->bots = p[off];
			}
		}
		return 1;
	}
	if (p[4] == 'I') {
		if (off < n) {
			it->version = p[off++];
		}
		Vellum_ReadStr(p, n, &off, it->name, sizeof(it->name));
		Vellum_ReadStr(p, n, &off, it->map, sizeof(it->map));
		Vellum_ReadStr(p, n, &off, it->gameDir, sizeof(it->gameDir));
		Vellum_ReadStr(p, n, &off, it->desc, sizeof(it->desc));
		if (off + 2 <= n) {
			it->appId = (uint32)(p[off] | (p[off + 1] << 8));
			off += 2;
		}
		if (off + 6 <= n) {
			it->players = p[off++];
			it->maxPlayers = p[off++];
			it->bots = p[off++];
			off++; /* type */
			off++; /* os */
			it->password = p[off++] != 0;
		}
		if (off < n) {
			it->secure = p[off] != 0;
		}
		return 1;
	}
	return 0;
}

static void Vellum_FailQuery(VellumQuery *q)
{
	void *cb = q->cb;
	void (*info_cb)(void *, int, const VellumGameServerItem *) = q->info_cb;
	void *info_user = q->info_user;
	int type = q->type;
	Vellum_QueryClose(q);
	if (info_cb != NULL) {
		info_cb(info_user, 0, NULL);
		return;
	}
	if (cb == NULL) {
		return;
	}
	if (type == VELLUM_Q_PING) {
		((SteamPingResponse *)cb)->ServerFailedToRespond();
	} else if (type == VELLUM_Q_PLAYERS) {
		((SteamPlayersResponse *)cb)->PlayersFailedToRespond();
	} else if (type == VELLUM_Q_RULES) {
		((SteamRulesResponse *)cb)->RulesFailedToRespond();
	}
}

static void Vellum_HandlePkt(VellumQuery *q, uint8 *p, int n)
{
	uint32 challenge;
	if (n >= 9 && p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF && p[4] == 'A') {
		memcpy(&challenge, p + 5, 4);
		q->challenged = 1;
		q->sent_ms = Vellum_NowMs();
		if (q->type == VELLUM_Q_PING) {
			Vellum_SendInfo(q, challenge, 1);
		} else if (q->type == VELLUM_Q_PLAYERS) {
			Vellum_SendPlayers(q, challenge);
		} else {
			Vellum_SendRules(q, challenge);
		}
		return;
	}
	if (q->type == VELLUM_Q_PING) {
		VellumGameServerItem item;
		void (*info_cb)(void *, int, const VellumGameServerItem *) = q->info_cb;
		void *info_user = q->info_user;
		SteamPingResponse *cb = (SteamPingResponse *)q->cb;
		Vellum_FillBase(&item, q->ip, q->port);
		if (!Vellum_ParseInfo(p, n, &item)) {
			Vellum_FailQuery(q);
			return;
		}
		item.ping = (int)(Vellum_NowMs() - q->sent_ms);
		if (item.ping < 1) {
			item.ping = 1;
		}
		Vellum_QueryClose(q);
		if (info_cb != NULL) {
			info_cb(info_user, 1, &item);
		} else if (cb != NULL) {
			cb->ServerResponded(item);
		}
		return;
	}
	if (q->type == VELLUM_Q_PLAYERS) {
		SteamPlayersResponse *cb = (SteamPlayersResponse *)q->cb;
		int off;
		int count;
		int i;
		if (n < 6 || p[4] != 'D') {
			Vellum_FailQuery(q);
			return;
		}
		count = p[5];
		off = 6;
		for (i = 0; i < count && off < n; i++) {
			char name[64];
			int32 score = 0;
			float duration = 0.0f;
			if (off < n) {
				off++; /* index */
			}
			Vellum_ReadStr(p, n, &off, name, sizeof(name));
			if (off + 8 <= n) {
				memcpy(&score, p + off, 4);
				off += 4;
				memcpy(&duration, p + off, 4);
				off += 4;
			}
			if (cb != NULL) {
				cb->AddPlayerToList(name, score, duration);
			}
		}
		Vellum_QueryClose(q);
		if (cb != NULL) {
			cb->PlayersRefreshComplete();
		}
		return;
	}
	if (q->type == VELLUM_Q_RULES) {
		SteamRulesResponse *cb = (SteamRulesResponse *)q->cb;
		int off;
		int count;
		int i;
		if (n < 7 || p[4] != 'E') {
			Vellum_QueryClose(q);
			if (cb != NULL) {
				cb->RulesRefreshComplete();
			}
			return;
		}
		count = p[5] | (p[6] << 8);
		off = 7;
		for (i = 0; i < count && off < n; i++) {
			char key[128];
			char val[256];
			Vellum_ReadStr(p, n, &off, key, sizeof(key));
			Vellum_ReadStr(p, n, &off, val, sizeof(val));
			if (cb != NULL && key[0] != '\0') {
				cb->RulesResponded(key, val);
			}
		}
		Vellum_QueryClose(q);
		if (cb != NULL) {
			cb->RulesRefreshComplete();
		}
	}
}

void Vellum_QueryThink()
{
	int i;
	uint8 buf[2048];
	int n;
	unsigned now = Vellum_NowMs();
	for (i = 0; i < VELLUM_QUERY_MAX; i++) {
		VellumQuery *q = &g_queries[i];
		if (!q->used) {
			continue;
		}
		n = Vellum_RecvFrom(q->sock, buf, (int)sizeof(buf));
		if (n > 0) {
			Vellum_HandlePkt(q, buf, n);
			continue;
		}
		if (now - q->sent_ms > VELLUM_QUERY_TIMEOUT) {
			Vellum_FailQuery(q);
		}
	}
}

HServerQuery Vellum_QueryPing(uint32 ip, uint16 port, void *cb)
{
	VellumQuery *q;
	if (cb == NULL) {
		return 0;
	}
	q = Vellum_QueryAlloc(VELLUM_Q_PING, ip, port, cb);
	if (q == NULL) {
		return 0;
	}
	Vellum_SendInfo(q, 0, 0);
	return q->id;
}

HServerQuery Vellum_QueryInfo(uint32 ip, uint16 port, void (*cb)(void *, int, const VellumGameServerItem *), void *user)
{
	VellumQuery *q;
	if (cb == NULL) {
		return 0;
	}
	q = Vellum_QueryAlloc(VELLUM_Q_PING, ip, port, NULL);
	if (q == NULL) {
		return 0;
	}
	q->info_cb = cb;
	q->info_user = user;
	Vellum_SendInfo(q, 0, 0);
	return q->id;
}

int Vellum_QueryBusyCount()
{
	int i;
	int n = 0;
	for (i = 0; i < VELLUM_QUERY_MAX; i++) {
		if (g_queries[i].used) {
			n++;
		}
	}
	return n;
}

HServerQuery Vellum_QueryPlayers(uint32 ip, uint16 port, void *cb)
{
	uint32 ch = 0xFFFFFFFF;
	VellumQuery *q = Vellum_QueryAlloc(VELLUM_Q_PLAYERS, ip, port, cb);
	if (q == NULL) {
		return 0;
	}
	Vellum_SendPlayers(q, ch);
	return q->id;
}

HServerQuery Vellum_QueryRules(uint32 ip, uint16 port, void *cb)
{
	uint32 ch = 0xFFFFFFFF;
	VellumQuery *q = Vellum_QueryAlloc(VELLUM_Q_RULES, ip, port, cb);
	if (q == NULL) {
		return 0;
	}
	Vellum_SendRules(q, ch);
	return q->id;
}

void Vellum_QueryCancel(HServerQuery id)
{
	int i;
	if (id == 0) {
		return;
	}
	for (i = 0; i < VELLUM_QUERY_MAX; i++) {
		if (g_queries[i].used && g_queries[i].id == id) {
			Vellum_QueryClose(&g_queries[i]);
			return;
		}
	}
}
