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
#include <winnls.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include "steam_http.h"

#ifdef _WIN32
#define Vellum_StrNicmp _strnicmp
#else
#define Vellum_StrNicmp strncasecmp
#endif

#define VELLUM_QUERY_MAX 256
#define VELLUM_QUERY_TIMEOUT 3000
#define VELLUM_MASTER_MAX 8
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
static void Vellum_HttpListThink();

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
	Vellum_HttpListThink();
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

struct VellumHttpList {
	int used;
	volatile int abort;
	volatile int done;
	int n;
	int cap;
	int read;
	uint32 *ips;
	uint16 *ports;
	char url[512];
	char country[8];
	VellumMasterCb cb;
	void *user;
#ifdef _WIN32
	HANDLE thread;
	CRITICAL_SECTION lock;
	HINTERNET ses;
	HINTERNET con;
	HINTERNET req;
	char hosta[256];
	INTERNET_PORT port;
	int https;
#else
	pthread_t thread;
	pthread_mutex_t lock;
#endif
	int lock_ready;
	int thread_ready;
};

static VellumMaster g_masters[VELLUM_MASTER_MAX];
static VellumLan g_lans[VELLUM_MASTER_MAX];
static VellumHttpList g_httplists[VELLUM_MASTER_MAX];

static void Vellum_MasterSend(VellumMaster *m);

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

static int Vellum_ResolveHost(const char *host, uint32 *ip)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	unsigned a = 0, b = 0, c = 0, d = 0;
	if (host == NULL || host[0] == '\0' || ip == NULL) {
		return 0;
	}
	if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
	    a <= 255 && b <= 255 && c <= 255 && d <= 255 &&
	    strchr(host, ':') == NULL && strspn(host, "0123456789.") == strlen(host)) {
		*ip = (a << 24) | (b << 16) | (c << 8) | d;
		return 1;
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
		return 0;
	}
	{
		struct sockaddr_in *in = (struct sockaddr_in *)res->ai_addr;
		*ip = ntohl(in->sin_addr.s_addr);
	}
	freeaddrinfo(res);
	return *ip != 0;
}

static int Vellum_UdpMasterStart(const char *host, uint16 port, const char *filter, VellumMasterCb cb, void *user)
{
	int i;
	VellumMaster *m = NULL;
	uint32 ip = 0;
	if (cb == NULL || host == NULL || !Vellum_ResolveHost(host, &ip)) {
		Vellum_Log("Master UDP resolve fail %s", host ? host : "");
		return 0;
	}
	for (i = 0; i < VELLUM_MASTER_MAX; i++) {
		if (!g_masters[i].used) {
			m = &g_masters[i];
			break;
		}
	}
	if (m == NULL) {
		return 0;
	}
	memset(m, 0, sizeof(*m));
	m->sock = Vellum_OpenUdp();
	if (m->sock == INVALID_SOCKET) {
		return 0;
	}
	m->used = 1;
	m->master_ip = ip;
	m->master_port = port ? port : 27010;
	m->cb = cb;
	m->user = user;
	strncpy(m->filter, filter ? filter : "", sizeof(m->filter) - 1);
	strncpy(m->seed, "0.0.0.0:0", sizeof(m->seed) - 1);
	Vellum_Log("Master UDP %s:%u filter=%s", host, (unsigned)m->master_port, m->filter);
	Vellum_MasterSend(m);
	return 1;
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

static int Vellum_ParseIpv4(const char *s, uint32 *ip)
{
	unsigned a = 0, b = 0, c = 0, d = 0;
	if (s == NULL || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
		return 0;
	}
	if (a > 255 || b > 255 || c > 255 || d > 255) {
		return 0;
	}
	*ip = (a << 24) | (b << 16) | (c << 8) | d;
	return 1;
}

static void Vellum_HttpListLock(VellumHttpList *h)
{
#ifdef _WIN32
	EnterCriticalSection(&h->lock);
#else
	pthread_mutex_lock(&h->lock);
#endif
}

static void Vellum_HttpListUnlock(VellumHttpList *h)
{
#ifdef _WIN32
	LeaveCriticalSection(&h->lock);
#else
	pthread_mutex_unlock(&h->lock);
#endif
}

#ifdef _WIN32
static void Vellum_HttpListAbortNet(VellumHttpList *h)
{
	HINTERNET req = NULL;
	HINTERNET con = NULL;
	HINTERNET ses = NULL;
	if (h == NULL) {
		return;
	}
	Vellum_HttpListLock(h);
	req = h->req;
	con = h->con;
	ses = h->ses;
	h->req = NULL;
	h->con = NULL;
	h->ses = NULL;
	Vellum_HttpListUnlock(h);
	if (req != NULL) {
		WinHttpCloseHandle(req);
	}
	if (con != NULL) {
		WinHttpCloseHandle(con);
	}
	if (ses != NULL) {
		WinHttpCloseHandle(ses);
	}
}

static void Vellum_HttpListSetNet(VellumHttpList *h, HINTERNET ses, HINTERNET con, HINTERNET req)
{
	if (h == NULL) {
		return;
	}
	Vellum_HttpListLock(h);
	h->ses = ses;
	h->con = con;
	h->req = req;
	Vellum_HttpListUnlock(h);
}
#endif

#ifndef _WIN32
static void Vellum_HttpListAbortNet(VellumHttpList *h)
{
	(void)h;
}
#endif

#ifdef _WIN32
static int Vellum_HttpListParseUrl(const char *url, int *https, char *hosta, size_t hostn,
                                  INTERNET_PORT *port, const char **path)
{
	const char *p;
	const char *slash;
	char *colon;
	size_t n;

	if (url == NULL || https == NULL || hosta == NULL || hostn == 0 || port == NULL || path == NULL) {
		return 0;
	}
	if (Vellum_StrNicmp(url, "https://", 8) == 0) {
		*https = 1;
		p = url + 8;
		*port = 443;
	} else if (Vellum_StrNicmp(url, "http://", 7) == 0) {
		*https = 0;
		p = url + 7;
		*port = 80;
	} else {
		return 0;
	}
	slash = strchr(p, '/');
	if (slash == NULL) {
		return 0;
	}
	n = (size_t)(slash - p);
	if (n == 0 || n >= hostn) {
		return 0;
	}
	memcpy(hosta, p, n);
	hosta[n] = '\0';
	colon = strchr(hosta, ':');
	if (colon != NULL) {
		*port = (INTERNET_PORT)atoi(colon + 1);
		*colon = '\0';
	}
	*path = slash;
	return hosta[0] != '\0';
}

static void Vellum_HttpListCloseReq(VellumHttpList *h)
{
	HINTERNET req = NULL;
	if (h == NULL) {
		return;
	}
	Vellum_HttpListLock(h);
	req = h->req;
	h->req = NULL;
	Vellum_HttpListUnlock(h);
	if (req != NULL) {
		WinHttpCloseHandle(req);
	}
}

static int Vellum_HttpListEnsureConn(VellumHttpList *h, const char *url, wchar_t *path, int pathn)
{
	char hosta[256];
	wchar_t host[256];
	wchar_t wua[] = L"Valve/Steam HTTP Client 1.0";
	const char *slash = NULL;
	HINTERNET ses;
	HINTERNET con;
	INTERNET_PORT port = 80;
	int https = 0;
	DWORD proto;
	DWORD ms;

	if (h == NULL || path == NULL || pathn <= 0) {
		return 0;
	}
	if (!Vellum_HttpListParseUrl(url, &https, hosta, sizeof(hosta), &port, &slash)) {
		return 0;
	}
	if (MultiByteToWideChar(CP_ACP, 0, slash, -1, path, pathn) <= 0) {
		return 0;
	}
	if (h->ses != NULL && h->con != NULL && !h->abort &&
	    h->https == https && h->port == port && strcmp(h->hosta, hosta) == 0) {
		return 1;
	}
	Vellum_HttpListAbortNet(h);
	if (MultiByteToWideChar(CP_ACP, 0, hosta, -1, host, (int)(sizeof(host) / sizeof(host[0]))) <= 0) {
		return 0;
	}
	/* NO_PROXY: IE/WPAD autodetection from inside hl.exe can stall WinINet/WinHTTP for seconds. */
	ses = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (ses == NULL) {
		Vellum_Log("HttpList WinHttpOpen err=%u", (unsigned)GetLastError());
		return 0;
	}
	Vellum_HttpListSetNet(h, ses, NULL, NULL);
	proto = 0x00000080u | 0x00000200u | 0x00000800u; /* TLS1 / 1.1 / 1.2 */
	WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof(proto));
	ms = 5000;
	WinHttpSetOption(ses, WINHTTP_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
	WinHttpSetOption(ses, WINHTTP_OPTION_SEND_TIMEOUT, &ms, sizeof(ms));
	WinHttpSetOption(ses, WINHTTP_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
	if (h->abort) {
		Vellum_HttpListAbortNet(h);
		return 0;
	}
	con = WinHttpConnect(ses, host, port, 0);
	if (con == NULL) {
		Vellum_Log("HttpList WinHttpConnect err=%u host=%s", (unsigned)GetLastError(), hosta);
		Vellum_HttpListAbortNet(h);
		return 0;
	}
	strncpy(h->hosta, hosta, sizeof(h->hosta) - 1);
	h->hosta[sizeof(h->hosta) - 1] = '\0';
	h->port = port;
	h->https = https;
	Vellum_HttpListSetNet(h, ses, con, NULL);
	Vellum_Log("HttpList conn keep-alive https=%d %s:%u", https, hosta, (unsigned)port);
	return 1;
}

static int Vellum_HttpListFetch(VellumHttpList *h, const char *url, char **out, int *outn)
{
	HINTERNET ses;
	HINTERNET con;
	HINTERNET req = NULL;
	wchar_t path[768];
	DWORD n = 0;
	DWORD cap = 0;
	char *body = NULL;
	int ok = 0;

	*out = NULL;
	*outn = 0;
	if (h == NULL || url == NULL || url[0] == '\0' || h->abort) {
		return 0;
	}
	if (!Vellum_HttpListEnsureConn(h, url, path, (int)(sizeof(path) / sizeof(path[0])))) {
		return 0;
	}
	Vellum_HttpListLock(h);
	ses = h->ses;
	con = h->con;
	Vellum_HttpListUnlock(h);
	if (ses == NULL || con == NULL) {
		return 0;
	}
	req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	                         h->https ? WINHTTP_FLAG_SECURE : 0);
	if (req == NULL) {
		Vellum_Log("HttpList WinHttpOpenRequest err=%u", (unsigned)GetLastError());
		Vellum_HttpListAbortNet(h);
		return 0;
	}
	Vellum_HttpListSetNet(h, ses, con, req);
	if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(req, NULL)) {
		Vellum_Log("HttpList WinHttp GET err=%u url=%.180s", (unsigned)GetLastError(), url);
		Vellum_HttpListAbortNet(h);
		free(body);
		return 0;
	}
	for (;;) {
		DWORD avail = 0;
		DWORD got = 0;
		char *nb;
		if (h->abort) {
			break;
		}
		if (!WinHttpQueryDataAvailable(req, &avail)) {
			Vellum_Log("HttpList WinHttp avail err=%u", (unsigned)GetLastError());
			break;
		}
		if (avail == 0) {
			ok = 1;
			break;
		}
		if (n + avail + 1 > cap) {
			DWORD ncap = cap ? cap * 2 : 65536;
			while (ncap < n + avail + 1) {
				ncap *= 2;
			}
			nb = (char *)realloc(body, ncap);
			if (nb == NULL) {
				break;
			}
			body = nb;
			cap = ncap;
		}
		if (!WinHttpReadData(req, body + n, avail, &got) || got == 0) {
			ok = 1;
			break;
		}
		n += got;
	}
	Vellum_HttpListCloseReq(h);
	if (ok && body != NULL) {
		body[n] = '\0';
		*out = body;
		*outn = (int)n;
		return 1;
	}
	free(body);
	return 0;
}
#else
static int Vellum_HttpListFetch(VellumHttpList *h, const char *url, char **out, int *outn)
{
	uint8 *body = NULL;
	uint32 n = 0;
	uint32 status = 0;
	char *text;
	*out = NULL;
	*outn = 0;
	if (h == NULL || url == NULL || url[0] == '\0') {
		return 0;
	}
	if (!Vellum_NetHttpGet(url, "VellumServerBrowser/1.0", 20, 0, &body, &n, &status, &h->abort) ||
	    body == NULL) {
		free(body);
		return 0;
	}
	text = (char *)realloc(body, (size_t)n + 1);
	if (text == NULL) {
		free(body);
		return 0;
	}
	text[n] = '\0';
	*out = text;
	*outn = (int)n;
	return 1;
}
#endif

static int Vellum_HttpListGrow(VellumHttpList *h, int need)
{
	uint32 *nips;
	uint16 *nports;
	int ncap;
	if (need <= h->cap) {
		return 1;
	}
	ncap = h->cap ? h->cap : 256;
	while (ncap < need) {
		if (ncap > 1000000) {
			return 0;
		}
		ncap *= 2;
	}
	nips = (uint32 *)realloc(h->ips, (size_t)ncap * sizeof(uint32));
	if (nips == NULL) {
		return 0;
	}
	nports = (uint16 *)realloc(h->ports, (size_t)ncap * sizeof(uint16));
	if (nports == NULL) {
		h->ips = nips;
		return 0;
	}
	h->ips = nips;
	h->ports = nports;
	h->cap = ncap;
	return 1;
}

static void Vellum_HttpListPush(VellumHttpList *h, uint32 ip, uint16 port)
{
	Vellum_HttpListLock(h);
	if (Vellum_HttpListGrow(h, h->n + 1)) {
		h->ips[h->n] = ip;
		h->ports[h->n] = port;
		h->n++;
	}
	Vellum_HttpListUnlock(h);
}

static int Vellum_CountSub(const char *s, const char *sub)
{
	int n = 0;
	size_t sl;
	if (s == NULL || sub == NULL || sub[0] == '\0') {
		return 0;
	}
	sl = strlen(sub);
	while ((s = strstr(s, sub)) != NULL) {
		n++;
		s += sl;
	}
	return n;
}

/* Optional JSON field. Missing / unknown => keep. Only explicit false/0 is offline. */
static int Vellum_JsonStatusOffline(const char *json, const char *ipkey)
{
	const char *obj = ipkey;
	const char *end;
	const char *s;
	if (json == NULL || ipkey == NULL || ipkey < json) {
		return 0;
	}
	while (obj > json && *obj != '{') {
		obj--;
	}
	end = strchr(obj, '}');
	s = strstr(obj, "\"status\":");
	if (s == NULL || (end != NULL && s > end)) {
		return 0;
	}
	s += 9;
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	if (end != NULL && s >= end) {
		return 0;
	}
	if (*s == '0' && (s[1] < '0' || s[1] > '9')) {
		return 1;
	}
	return Vellum_StrNicmp(s, "false", 5) == 0;
}

static int Vellum_JsonCountryMismatch(const char *json, const char *ipkey, const char *want)
{
	const char *obj = ipkey;
	const char *end;
	const char *s;
	char got[8];
	int n = 0;
	if (want == NULL || want[0] == '\0' || json == NULL || ipkey == NULL) {
		return 0;
	}
	while (obj > json && *obj != '{') {
		obj--;
	}
	end = strchr(obj, '}');
	s = strstr(obj, "\"country\":\"");
	if (s == NULL || (end != NULL && s > end)) {
		return 0;
	}
	s += 11;
	while (*s && *s != '"' && n < (int)sizeof(got) - 1) {
		got[n++] = *s++;
	}
	got[n] = '\0';
	if (n != 2) {
		return 0;
	}
	return Vellum_StrNicmp(got, want, 2) != 0;
}

static void Vellum_StrRemove(char *s, const char *tok)
{
	char *p;
	size_t n;
	if (s == NULL || tok == NULL || tok[0] == '\0') {
		return;
	}
	n = strlen(tok);
	while ((p = strstr(s, tok)) != NULL) {
		memmove(p, p + n, strlen(p + n) + 1);
	}
}

static void Vellum_StoreCountry(char *out, int outn, const char *cc)
{
	char a;
	char b;
	if (out == NULL || outn < 3 || cc == NULL) {
		return;
	}
	a = cc[0];
	b = cc[1];
	if (a >= 'a' && a <= 'z') {
		a = (char)(a - 32);
	}
	if (b >= 'a' && b <= 'z') {
		b = (char)(b - 32);
	}
	if (a < 'A' || a > 'Z' || b < 'A' || b > 'Z') {
		return;
	}
	out[0] = a;
	out[1] = b;
	out[2] = '\0';
}

static void Vellum_CountryFromOs(char *out, int outn)
{
#ifdef _WIN32
	GEOID id;
	wchar_t w[8];
	char cc[8];
	id = GetUserGeoID(GEOCLASS_NATION);
	if (id == GEOID_NOT_AVAILABLE) {
		return;
	}
	if (GetGeoInfoW(id, GEO_ISO2, w, (int)(sizeof(w) / sizeof(w[0])), 0) <= 0) {
		return;
	}
	if (WideCharToMultiByte(CP_ACP, 0, w, -1, cc, (int)sizeof(cc), NULL, NULL) <= 0) {
		return;
	}
	Vellum_StoreCountry(out, outn, cc);
#else
	const char *lang = getenv("LC_ALL");
	if (lang == NULL || lang[0] == '\0') {
		lang = getenv("LANG");
	}
	if (lang != NULL && lang[0] != '\0' && lang[1] != '\0' && lang[2] == '_') {
		char cc[4];
		cc[0] = lang[0];
		cc[1] = lang[1];
		cc[2] = '\0';
		Vellum_StoreCountry(out, outn, cc);
	}
#endif
}

static void Vellum_DetectCountry(VellumHttpList *h)
{
	if (h == NULL) {
		return;
	}
	h->country[0] = '\0';
	Vellum_CountryFromOs(h->country, (int)sizeof(h->country));
	Vellum_Log("HttpList country=%s", h->country[0] ? h->country : "(none)");
}

static int Vellum_HttpListParse(const char *json, VellumHttpList *h, int *offline)
{
	const char *p = json;
	int added = 0;
	if (offline) {
		*offline = 0;
	}
	if (json == NULL) {
		return 0;
	}
	while ((p = strstr(p, "\"ip\":\"")) != NULL) {
		char ipstr[32];
		const char *q;
		int n = 0;
		uint32 ip = 0;
		unsigned port = 0;
		if (Vellum_JsonStatusOffline(json, p)) {
			if (offline) {
				(*offline)++;
			}
			p += 6;
			continue;
		}
		if (Vellum_JsonCountryMismatch(json, p, h->country)) {
			p += 6;
			continue;
		}
		p += 6;
		while (*p && *p != '"' && n < (int)sizeof(ipstr) - 1) {
			ipstr[n++] = *p++;
		}
		ipstr[n] = '\0';
		if (!Vellum_ParseIpv4(ipstr, &ip)) {
			continue;
		}
		q = strstr(p, "\"port\":");
		if (q == NULL || q > p + 160) {
			continue;
		}
		port = (unsigned)atoi(q + 7);
		if (port == 0 || port > 65535) {
			continue;
		}
		if (h->abort) {
			break;
		}
		Vellum_HttpListPush(h, ip, port);
		added++;
	}
	return added;
}

static int Vellum_HttpListPageSize(const char *url)
{
	const char *s;
	int n;
	if (url == NULL) {
		return 0;
	}
	s = strstr(url, "limit=");
	if (s == NULL) {
		return 0;
	}
	n = atoi(s + 6);
	if (n < 1) {
		return 0;
	}
	if (n > 1000) {
		n = 1000;
	}
	return n;
}

static void Vellum_HttpListMakeUrl(char *out, int outn, const char *tmpl, int offset, const char *country)
{
	char tmp[768];
	const char *ph;
	if (tmpl == NULL || out == NULL || outn <= 0) {
		return;
	}
	strncpy(tmp, tmpl, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	if (country == NULL || country[0] == '\0') {
		Vellum_StrRemove(tmp, "&country={country}");
		Vellum_StrRemove(tmp, "country={country}&");
		Vellum_StrRemove(tmp, "country={country}");
	} else {
		ph = strstr(tmp, "{country}");
		if (ph != NULL) {
			char filled[768];
#ifdef _WIN32
			_snprintf(filled, sizeof(filled), "%.*s%s%s", (int)(ph - tmp), tmp, country, ph + 9);
#else
			snprintf(filled, sizeof(filled), "%.*s%s%s", (int)(ph - tmp), tmp, country, ph + 9);
#endif
			filled[sizeof(filled) - 1] = '\0';
			strncpy(tmp, filled, sizeof(tmp) - 1);
			tmp[sizeof(tmp) - 1] = '\0';
		}
	}
	ph = strstr(tmp, "{offset}");
	if (ph == NULL) {
		strncpy(out, tmp, (size_t)outn - 1);
		out[outn - 1] = '\0';
		return;
	}
#ifdef _WIN32
	_snprintf(out, (size_t)outn, "%.*s%d%s", (int)(ph - tmp), tmp, offset, ph + 8);
#else
	snprintf(out, (size_t)outn, "%.*s%d%s", (int)(ph - tmp), tmp, offset, ph + 8);
#endif
	out[outn - 1] = '\0';
}

#ifdef _WIN32
static DWORD WINAPI Vellum_HttpListWorker(LPVOID param)
#else
static void *Vellum_HttpListWorker(void *param)
#endif
{
	VellumHttpList *h = (VellumHttpList *)param;
	int offset;
	int added_total = 0;
	int step = Vellum_HttpListPageSize(h->url);
	int stride = step > 0 ? step : 100;
	int paged = strstr(h->url, "{offset}") != NULL;
	if (strstr(h->url, "{country}") != NULL) {
		Vellum_DetectCountry(h);
	}
	for (offset = 0; offset < 100000 && !h->abort; offset += stride) {
		char url[768];
		char *body = NULL;
		int n = 0;
		int added;
		int offline = 0;
		int n_ip;
		int n_conn;
		int page_n;
		Vellum_HttpListMakeUrl(url, (int)sizeof(url), h->url, offset, h->country);
		if (!Vellum_HttpListFetch(h, url, &body, &n) || body == NULL) {
			Vellum_HttpListAbortNet(h);
			if (h->abort || !Vellum_HttpListFetch(h, url, &body, &n) || body == NULL) {
				Vellum_Log("HttpList fetch fail offset=%d url=%.180s", offset, url);
				break;
			}
		}
		n_ip = Vellum_CountSub(body, "\"ip\":\"");
		n_conn = Vellum_CountSub(body, "\"connect\"");
		page_n = n_ip > n_conn ? n_ip : n_conn;
		added = Vellum_HttpListParse(body, h, &offline);
		free(body);
		added_total += added;
		if (offset == 0 || page_n == 0 || (step > 0 && page_n < step) || (offset % 1000) == 0) {
			Vellum_Log("HttpList page offset=%d json=%d parsed=%d offline=%d total=%d step=%d",
			           offset, page_n, added, offline, h->n, step);
		}
		if (!paged || page_n == 0 || (step > 0 && page_n < step)) {
			break;
		}
		if (offline > 0 && added == 0) {
			break;
		}
	}
	Vellum_HttpListAbortNet(h);
	Vellum_HttpListLock(h);
	h->done = 1;
	Vellum_HttpListUnlock(h);
	Vellum_Log("HttpList done added=%d abort=%d", added_total, h->abort);
#ifdef _WIN32
	return 0;
#else
	return NULL;
#endif
}

static void Vellum_HttpListFinish(VellumHttpList *h)
{
	if (h->lock_ready) {
#ifdef _WIN32
		DeleteCriticalSection(&h->lock);
#else
		pthread_mutex_destroy(&h->lock);
#endif
		h->lock_ready = 0;
	}
	free(h->ips);
	free(h->ports);
	h->ips = NULL;
	h->ports = NULL;
	h->cap = 0;
	h->n = 0;
	h->read = 0;
	h->used = 0;
	h->cb = NULL;
	h->user = NULL;
	h->thread_ready = 0;
#ifdef _WIN32
	h->thread = NULL;
	h->ses = NULL;
	h->con = NULL;
	h->req = NULL;
#endif
}

static void Vellum_HttpListClose(VellumHttpList *h)
{
	h->abort = 1;
	h->cb = NULL;
	h->user = NULL;
#ifdef _WIN32
	Vellum_HttpListAbortNet(h);
	if (h->thread_ready) {
		if (WaitForSingleObject(h->thread, 0) != WAIT_OBJECT_0) {
			return;
		}
		CloseHandle(h->thread);
		h->thread = NULL;
		h->thread_ready = 0;
	}
#else
	if (h->thread_ready) {
		pthread_join(h->thread, NULL);
		h->thread_ready = 0;
	}
#endif
	Vellum_HttpListFinish(h);
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
		Vellum_HttpListLock(h);
		{
			int drained = 0;
			while (h->read < h->n && drained < 256) {
				uint32 ip = h->ips[h->read];
				uint16 port = h->ports[h->read];
				h->read++;
				drained++;
				Vellum_HttpListUnlock(h);
				if (h->cb != NULL) {
					h->cb(h->user, ip, port, 0);
				}
				Vellum_HttpListLock(h);
			}
		}
		done = h->done && h->read >= h->n;
		Vellum_HttpListUnlock(h);
#ifdef _WIN32
		if (!done && h->abort && h->thread_ready &&
		    WaitForSingleObject(h->thread, 0) == WAIT_OBJECT_0) {
			done = 1;
		}
#endif
		if (done) {
			VellumMasterCb cb = h->cb;
			void *user = h->user;
#ifdef _WIN32
			if (h->thread_ready) {
				WaitForSingleObject(h->thread, 0);
				CloseHandle(h->thread);
				h->thread = NULL;
				h->thread_ready = 0;
			}
			Vellum_HttpListFinish(h);
#else
			Vellum_HttpListClose(h);
#endif
			if (cb != NULL) {
				cb(user, 0, 0, 1);
			}
		}
	}
}

static int Vellum_HttpListStart(const char *url, VellumMasterCb cb, void *user)
{
	int i;
	VellumHttpList *h = NULL;
	if (url == NULL || url[0] == '\0') {
		return 0;
	}
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
#ifdef _WIN32
	InitializeCriticalSection(&h->lock);
#else
	if (pthread_mutex_init(&h->lock, NULL) != 0) {
		return 0;
	}
#endif
	h->lock_ready = 1;
	h->used = 1;
	h->cb = cb;
	h->user = user;
	strncpy(h->url, url, sizeof(h->url) - 1);
#ifdef _WIN32
	h->thread = CreateThread(NULL, 0, Vellum_HttpListWorker, h, 0, NULL);
	if (h->thread == NULL) {
		DeleteCriticalSection(&h->lock);
		h->lock_ready = 0;
		h->used = 0;
		return 0;
	}
#else
	if (pthread_create(&h->thread, NULL, Vellum_HttpListWorker, h) != 0) {
		pthread_mutex_destroy(&h->lock);
		h->lock_ready = 0;
		h->used = 0;
		return 0;
	}
#endif
	h->thread_ready = 1;
	Vellum_Log("HttpList start %s", h->url);
	return 1;
}

static int Vellum_SplitHostPort(const char *addr, char *host, size_t hostn, uint16 *port)
{
	const char *colon;
	size_t n;
	if (addr == NULL || host == NULL || hostn == 0 || port == NULL) {
		return 0;
	}
	while (*addr == ' ' || *addr == '\t') {
		addr++;
	}
	colon = strrchr(addr, ':');
	if (colon != NULL && colon != addr && strchr(addr, ':') == colon) {
		n = (size_t)(colon - addr);
		if (n >= hostn) {
			n = hostn - 1;
		}
		memcpy(host, addr, n);
		host[n] = '\0';
		*port = (uint16)strtoul(colon + 1, NULL, 10);
		if (*port == 0) {
			*port = 27010;
		}
		return host[0] != '\0';
	}
	strncpy(host, addr, hostn - 1);
	host[hostn - 1] = '\0';
	*port = 27010;
	return host[0] != '\0';
}

int Vellum_MasterAdd(const char *address, const char *filter, VellumMasterCb cb, void *user)
{
	char host[256];
	uint16 port = 27010;
	Vellum_QueryInit();
	if (cb == NULL || address == NULL || address[0] == '\0') {
		return 0;
	}
	if (Vellum_StrNicmp(address, "http://", 7) == 0 || Vellum_StrNicmp(address, "https://", 8) == 0) {
		(void)filter;
		return Vellum_HttpListStart(address, cb, user);
	}
	if (!Vellum_SplitHostPort(address, host, sizeof(host), &port)) {
		return 0;
	}
	return Vellum_UdpMasterStart(host, port, filter, cb, user);
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
		if (g_httplists[i].used && g_httplists[i].user == user) {
			Vellum_HttpListClose(&g_httplists[i]);
		}
	}
}
