#include "steam_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wininet.h>
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

#define VELLUM_QUERY_MAX 24
#define VELLUM_QUERY_TIMEOUT 3000
#define VELLUM_MASTER_MAX 4
#define VELLUM_MASTER_TIMEOUT 4000
#define VELLUM_LAN_TIMEOUT 2000

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

extern void Vellum_Log(const char *fmt, ...);

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

static int Vellum_RecvFromAddr(SOCKET s, void *data, int n, uint32 *ip, uint16 *port)
{
	struct sockaddr_in a;
#ifdef _WIN32
	int alen = (int)sizeof(a);
#else
	socklen_t alen = sizeof(a);
#endif
	int got = recvfrom(s, (char *)data, n, 0, (struct sockaddr *)&a, &alen);
	if (got > 0) {
		if (ip) {
			*ip = ntohl(a.sin_addr.s_addr);
		}
		if (port) {
			*port = ntohs(a.sin_port);
		}
	}
	return got;
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

static void Vellum_MasterThink();
static void Vellum_LanThink();
#ifdef _WIN32
static void Vellum_HttpListThink();
#endif

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
	Vellum_MasterThink();
	Vellum_LanThink();
#ifdef _WIN32
	Vellum_HttpListThink();
#endif
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

struct VellumMaster {
	int used;
	SOCKET sock;
	uint32 master_ip;
	uint16 master_port;
	char filter[512];
	char seed[32];
	unsigned sent_ms;
	int tries;
	VellumMasterCb cb;
	void *user;
};

struct VellumLan {
	int used;
	SOCKET sock;
	unsigned sent_ms;
	VellumMasterCb cb;
	void *user;
};

#ifdef _WIN32
struct VellumHttpList {
	int used;
	int abort;
	int done;
	int n;
	int read;
	uint32 ips[512];
	uint16 ports[512];
	VellumMasterCb cb;
	void *user;
	HANDLE thread;
	CRITICAL_SECTION lock;
	int lock_ready;
};
#else
struct VellumHttpList {
	int used;
	VellumMasterCb cb;
	void *user;
};
#endif

static VellumMaster g_masters[VELLUM_MASTER_MAX];
static VellumLan g_lans[VELLUM_MASTER_MAX];
static VellumHttpList g_httplists[VELLUM_MASTER_MAX];
static uint32 g_master_ip;
static int g_master_ip_ok;

static uint32 Vellum_MasterResolve()
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	uint32 ip = 0;
	static const uint32 fallbacks[] = {
		(208u << 24) | (64u << 16) | (200u << 8) | 52u,
		(208u << 24) | (64u << 16) | (200u << 8) | 39u,
		(208u << 24) | (64u << 16) | (200u << 8) | 65u
	};
	if (g_master_ip_ok) {
		return g_master_ip;
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo("hl2master.steampowered.com", NULL, &hints, &res) == 0 && res != NULL) {
		struct sockaddr_in *in = (struct sockaddr_in *)res->ai_addr;
		ip = ntohl(in->sin_addr.s_addr);
		freeaddrinfo(res);
	}
	if (ip == 0) {
		ip = fallbacks[0];
	}
	g_master_ip = ip;
	g_master_ip_ok = 1;
	return ip;
}

static void Vellum_MasterClose(VellumMaster *m)
{
	if (m->sock != INVALID_SOCKET) {
		closesocket(m->sock);
		m->sock = INVALID_SOCKET;
	}
	m->used = 0;
	m->cb = NULL;
	m->user = NULL;
}

static void Vellum_LanClose(VellumLan *l)
{
	if (l->sock != INVALID_SOCKET) {
		closesocket(l->sock);
		l->sock = INVALID_SOCKET;
	}
	l->used = 0;
	l->cb = NULL;
	l->user = NULL;
}

static void Vellum_MasterSend(VellumMaster *m)
{
	char pkt[600];
	int n = 0;
	int flen;
	pkt[n++] = '1';
	pkt[n++] = (char)0xFF;
	flen = (int)strlen(m->seed);
	memcpy(pkt + n, m->seed, (size_t)flen + 1);
	n += flen + 1;
	flen = (int)strlen(m->filter);
	memcpy(pkt + n, m->filter, (size_t)flen + 1);
	n += flen + 1;
	Vellum_SendTo(m->sock, m->master_ip, m->master_port, pkt, n);
	m->sent_ms = Vellum_NowMs();
	m->tries++;
}

static void Vellum_MasterFinish(VellumMaster *m)
{
	VellumMasterCb cb = m->cb;
	void *user = m->user;
	Vellum_MasterClose(m);
	if (cb != NULL) {
		cb(user, 0, 0, 1);
	}
}

static void Vellum_MasterThink()
{
	int i;
	uint8 buf[2048];
	int n;
	unsigned now = Vellum_NowMs();
	for (i = 0; i < VELLUM_MASTER_MAX; i++) {
		VellumMaster *m = &g_masters[i];
		int off;
		int more;
		uint32 last_ip = 0;
		uint16 last_port = 0;
		if (!m->used) {
			continue;
		}
		n = Vellum_RecvFrom(m->sock, buf, (int)sizeof(buf));
		if (n < 6) {
			if (now - m->sent_ms > VELLUM_MASTER_TIMEOUT) {
				if (m->tries < 3) {
					Vellum_MasterSend(m);
				} else {
					Vellum_Log("Master timeout filter=%s", m->filter);
					Vellum_MasterFinish(m);
				}
			}
			continue;
		}
		if (buf[0] != 0xFF || buf[1] != 0xFF || buf[2] != 0xFF || buf[3] != 0xFF || buf[4] != 0x66) {
			continue;
		}
		off = 6;
		more = 0;
		while (off + 6 <= n) {
			uint32 nip;
			uint16 nport;
			uint32 ip;
			uint16 port;
			memcpy(&nip, buf + off, 4);
			memcpy(&nport, buf + off + 4, 2);
			off += 6;
			ip = ntohl(nip);
			port = ntohs(nport);
			if (ip == 0 && port == 0) {
				more = 0;
				break;
			}
			last_ip = ip;
			last_port = port;
			more = 1;
			if (m->cb != NULL) {
				m->cb(m->user, ip, port, 0);
			}
		}
		if (!m->used) {
			continue;
		}
		if (!more) {
			Vellum_MasterFinish(m);
			continue;
		}
#ifdef _WIN32
		_snprintf(m->seed, sizeof(m->seed), "%u.%u.%u.%u:%u",
		          (last_ip >> 24) & 255, (last_ip >> 16) & 255, (last_ip >> 8) & 255, last_ip & 255,
		          (unsigned)last_port);
#else
		snprintf(m->seed, sizeof(m->seed), "%u.%u.%u.%u:%u",
		         (last_ip >> 24) & 255, (last_ip >> 16) & 255, (last_ip >> 8) & 255, last_ip & 255,
		         (unsigned)last_port);
#endif
		m->seed[sizeof(m->seed) - 1] = '\0';
		m->tries = 0;
		Vellum_MasterSend(m);
	}
}

static void Vellum_LanSendAll(VellumLan *l)
{
	uint8 pkt[64];
	uint8 oldq[12];
	int n = 0;
	int port;
	memset(pkt, 0xFF, 4);
	n = 4;
	pkt[n++] = 'T';
	memcpy(pkt + n, "Source Engine Query", 19);
	n += 19;
	pkt[n++] = 0;
	memset(oldq, 0xFF, 4);
	memcpy(oldq + 4, "details", 7);
	oldq[11] = 0;
	for (port = 27015; port <= 27020; port++) {
		Vellum_SendTo(l->sock, 0xFFFFFFFFu, (uint16)port, pkt, n);
		Vellum_SendTo(l->sock, 0xFFFFFFFFu, (uint16)port, oldq, 12);
	}
}

static void Vellum_LanThink()
{
	int i;
	uint8 buf[2048];
	int n;
	unsigned now = Vellum_NowMs();
	for (i = 0; i < VELLUM_MASTER_MAX; i++) {
		VellumLan *l = &g_lans[i];
		uint32 ip = 0;
		uint16 port = 0;
		if (!l->used) {
			continue;
		}
		while ((n = Vellum_RecvFromAddr(l->sock, buf, (int)sizeof(buf), &ip, &port)) > 0) {
			if (n >= 5 && (buf[4] == 'I' || buf[4] == 'm') && l->cb != NULL) {
				l->cb(l->user, ip, port, 0);
			}
		}
		if (now - l->sent_ms > VELLUM_LAN_TIMEOUT) {
			VellumMasterCb cb = l->cb;
			void *user = l->user;
			Vellum_LanClose(l);
			if (cb != NULL) {
				cb(user, 0, 0, 1);
			}
		}
	}
}

#ifdef _WIN32
static int Vellum_ParseDottedAddr(const char *s, uint32 *ip, uint16 *port)
{
	unsigned a = 0, b = 0, c = 0, d = 0, p = 0;
	if (s == NULL || sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p) != 5) {
		return 0;
	}
	if (a > 255 || b > 255 || c > 255 || d > 255 || p > 65535) {
		return 0;
	}
	*ip = (a << 24) | (b << 16) | (c << 8) | d;
	*port = (uint16)p;
	return 1;
}

static int Vellum_HttpListFetch(const char *url, char **out, int *outn)
{
	HINTERNET ses;
	HINTERNET req;
	char buf[4096];
	DWORD got;
	char *body = NULL;
	int n = 0;
	int cap = 0;
	*out = NULL;
	*outn = 0;
	ses = InternetOpenA("VellumServerBrowser/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	if (ses == NULL) {
		return 0;
	}
	req = InternetOpenUrlA(ses, url, NULL, 0,
	                       INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE |
	                       INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES, 0);
	if (req == NULL) {
		InternetCloseHandle(ses);
		return 0;
	}
	while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0) {
		if (n + (int)got + 1 > cap) {
			int ncap = cap ? cap * 2 : 65536;
			char *nb;
			while (ncap < n + (int)got + 1) {
				ncap *= 2;
			}
			nb = (char *)realloc(body, (size_t)ncap);
			if (nb == NULL) {
				free(body);
				InternetCloseHandle(req);
				InternetCloseHandle(ses);
				return 0;
			}
			body = nb;
			cap = ncap;
		}
		memcpy(body + n, buf, got);
		n += (int)got;
	}
	InternetCloseHandle(req);
	InternetCloseHandle(ses);
	if (body == NULL) {
		return 0;
	}
	body[n] = '\0';
	*out = body;
	*outn = n;
	return 1;
}

static void Vellum_HttpListPush(VellumHttpList *h, uint32 ip, uint16 port)
{
	EnterCriticalSection(&h->lock);
	if (h->n < 512) {
		h->ips[h->n] = ip;
		h->ports[h->n] = port;
		h->n++;
	}
	LeaveCriticalSection(&h->lock);
}

static int Vellum_HttpListParse(const char *json, VellumHttpList *h)
{
	const char *p = json;
	int added = 0;
	while ((p = strstr(p, "\"connect\"")) != NULL) {
		char addr[64];
		int n = 0;
		uint32 ip = 0;
		uint16 port = 0;
		p += 9;
		while (*p && *p != '"') {
			p++;
		}
		if (*p != '"') {
			break;
		}
		p++;
		while (*p && *p != '"' && n < (int)sizeof(addr) - 1) {
			addr[n++] = *p++;
		}
		addr[n] = '\0';
		if (h->abort) {
			break;
		}
		if (Vellum_ParseDottedAddr(addr, &ip, &port)) {
			Vellum_HttpListPush(h, ip, port);
			added++;
		}
	}
	return added;
}

static DWORD WINAPI Vellum_HttpListWorker(LPVOID param)
{
	VellumHttpList *h = (VellumHttpList *)param;
	int offset;
	int added_total = 0;
	for (offset = 0; offset < 500 && !h->abort; offset += 100) {
		char url[256];
		char *body = NULL;
		int n = 0;
		int added;
		_snprintf(url, sizeof(url),
		          "https://api.gamemonitoring.net/servers?game=10&limit=100&offset=%d", offset);
		url[sizeof(url) - 1] = '\0';
		if (!Vellum_HttpListFetch(url, &body, &n) || body == NULL) {
			break;
		}
		added = Vellum_HttpListParse(body, h);
		free(body);
		added_total += added;
		if (added < 100) {
			break;
		}
	}
	EnterCriticalSection(&h->lock);
	h->done = 1;
	LeaveCriticalSection(&h->lock);
	Vellum_Log("HttpList done added=%d abort=%d", added_total, h->abort);
	return 0;
}

static void Vellum_HttpListClose(VellumHttpList *h)
{
	h->abort = 1;
	if (h->thread) {
		WaitForSingleObject(h->thread, 8000);
		CloseHandle(h->thread);
		h->thread = NULL;
	}
	if (h->lock_ready) {
		DeleteCriticalSection(&h->lock);
		h->lock_ready = 0;
	}
	h->used = 0;
	h->cb = NULL;
	h->user = NULL;
}

static void Vellum_HttpListThink()
{
	int i;
	for (i = 0; i < VELLUM_MASTER_MAX; i++) {
		VellumHttpList *h = &g_httplists[i];
		int done = 0;
		if (!h->used) {
			continue;
		}
		EnterCriticalSection(&h->lock);
		while (h->read < h->n) {
			uint32 ip = h->ips[h->read];
			uint16 port = h->ports[h->read];
			h->read++;
			LeaveCriticalSection(&h->lock);
			if (h->cb != NULL) {
				h->cb(h->user, ip, port, 0);
			}
			EnterCriticalSection(&h->lock);
		}
		done = h->done;
		LeaveCriticalSection(&h->lock);
		if (done) {
			VellumMasterCb cb = h->cb;
			void *user = h->user;
			Vellum_HttpListClose(h);
			if (cb != NULL) {
				cb(user, 0, 0, 1);
			}
		}
	}
}

static int Vellum_HttpListStart(VellumMasterCb cb, void *user)
{
	int i;
	VellumHttpList *h = NULL;
	for (i = 0; i < VELLUM_MASTER_MAX; i++) {
		if (!g_httplists[i].used) {
			h = &g_httplists[i];
			break;
		}
	}
	if (h == NULL) {
		return 0;
	}
	memset(h, 0, sizeof(*h));
	InitializeCriticalSection(&h->lock);
	h->lock_ready = 1;
	h->used = 1;
	h->cb = cb;
	h->user = user;
	h->thread = CreateThread(NULL, 0, Vellum_HttpListWorker, h, 0, NULL);
	if (h->thread == NULL) {
		DeleteCriticalSection(&h->lock);
		h->lock_ready = 0;
		h->used = 0;
		return 0;
	}
	Vellum_Log("HttpList start");
	return 1;
}
#endif

int Vellum_MasterStart(const char *filter, VellumMasterCb cb, void *user)
{
	Vellum_QueryInit();
	if (cb == NULL) {
		return 0;
	}
	(void)filter;
	Vellum_MasterCancel(user);
#ifdef _WIN32
	return Vellum_HttpListStart(cb, user);
#else
	return 0;
#endif
}

int Vellum_LanStart(VellumMasterCb cb, void *user)
{
	int i;
	VellumLan *l = NULL;
	int one = 1;
	Vellum_QueryInit();
	if (!g_query_wsa || cb == NULL) {
		return 0;
	}
	Vellum_MasterCancel(user);
	for (i = 0; i < VELLUM_MASTER_MAX; i++) {
		if (!g_lans[i].used) {
			l = &g_lans[i];
			break;
		}
	}
	if (l == NULL) {
		return 0;
	}
	memset(l, 0, sizeof(*l));
	l->sock = Vellum_OpenUdp();
	if (l->sock == INVALID_SOCKET) {
		return 0;
	}
	setsockopt(l->sock, SOL_SOCKET, SO_BROADCAST, (const char *)&one, (int)sizeof(one));
	l->used = 1;
	l->cb = cb;
	l->user = user;
	l->sent_ms = Vellum_NowMs();
	Vellum_LanSendAll(l);
	Vellum_Log("LAN start");
	return 1;
}

void Vellum_MasterCancel(void *user)
{
	int i;
	if (user == NULL) {
		return;
	}
	for (i = 0; i < VELLUM_MASTER_MAX; i++) {
		if (g_masters[i].used && g_masters[i].user == user) {
			Vellum_MasterClose(&g_masters[i]);
		}
		if (g_lans[i].used && g_lans[i].user == user) {
			Vellum_LanClose(&g_lans[i]);
		}
#ifdef _WIN32
		if (g_httplists[i].used && g_httplists[i].user == user) {
			Vellum_HttpListClose(&g_httplists[i]);
		}
#endif
	}
}
