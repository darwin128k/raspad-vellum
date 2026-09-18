#include "steam_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#endif

#define VELLUM_HTTP_MAX 8
#define VELLUM_HTTP_BODY_MAX (48 * 1024 * 1024)
#define VELLUM_HTTP_HDR_MAX 8

enum {
	k_EHTTPMethodGET = 1,
	k_EHTTPMethodHEAD = 2,
	k_EHTTPMethodPOST = 3
};

enum {
	k_iSteamUtilsCallbacks = 700,
	k_iSteamHTTPCallbacks = 2100
};

#pragma pack(push, 8)
struct HTTPRequestCompleted_t {
	HTTPRequestHandle m_hRequest;
	uint64 m_ulContextValue;
	bool m_bRequestSuccessful;
	int m_eStatusCode;
	uint32 m_unBodySize;
};

struct SteamAPICallCompleted_t {
	SteamAPICall_t m_hAsyncCall;
	int m_iCallback;
	uint32 m_cubParam;
};
#pragma pack(pop)

struct VellumHttpHdr {
	char name[64];
	char value[256];
};

struct VellumHttpReq {
	int used;
	int sent;
	int done;
	int notified;
	int timed_out;
	int abort;
	HTTPRequestHandle id;
	SteamAPICall_t call;
	uint64 context;
	int method;
	uint32 timeout_ms;
	uint32 status;
	uint32 body_size;
	uint32 body_got;
	float progress;
	bool ok;
	char url[1024];
	char ua[128];
	int nhdr;
	VellumHttpHdr hdrs[VELLUM_HTTP_HDR_MAX];
	uint8 *body;
#ifdef _WIN32
	HANDLE thread;
	CRITICAL_SECTION lock;
	int lock_ready;
#endif
};

static VellumHttpReq g_http_reqs[VELLUM_HTTP_MAX];
static HTTPRequestHandle g_http_next = 1;
static SteamAPICall_t g_http_call_next = 1;
#ifdef _WIN32
static CRITICAL_SECTION g_http_cs;
static int g_http_cs_ready;
#endif

static void Vellum_HttpLockInit()
{
#ifdef _WIN32
	if (!g_http_cs_ready) {
		InitializeCriticalSection(&g_http_cs);
		g_http_cs_ready = 1;
	}
#endif
}

static VellumHttpReq *Vellum_HttpFind(HTTPRequestHandle h)
{
	int i;
	if (h == 0) {
		return NULL;
	}
	for (i = 0; i < VELLUM_HTTP_MAX; i++) {
		if (g_http_reqs[i].used && g_http_reqs[i].id == h) {
			return &g_http_reqs[i];
		}
	}
	return NULL;
}

static VellumHttpReq *Vellum_HttpFindCall(SteamAPICall_t call)
{
	int i;
	if (call == 0) {
		return NULL;
	}
	for (i = 0; i < VELLUM_HTTP_MAX; i++) {
		if (g_http_reqs[i].used && g_http_reqs[i].call == call) {
			return &g_http_reqs[i];
		}
	}
	return NULL;
}

#ifdef _WIN32
static void Vellum_HttpBuildHeaders(VellumHttpReq *r, char *out, int outsz)
{
	int i;
	int n = 0;
	out[0] = '\0';
	for (i = 0; i < r->nhdr && n < outsz - 4; i++) {
		n += _snprintf(out + n, (size_t)(outsz - n), "%s: %s\r\n", r->hdrs[i].name, r->hdrs[i].value);
		if (n < 0 || n >= outsz) {
			out[outsz - 1] = '\0';
			return;
		}
	}
}

static DWORD WINAPI Vellum_HttpWorker(LPVOID param)
{
	VellumHttpReq *r = (VellumHttpReq *)param;
	HINTERNET ses = NULL;
	HINTERNET req = NULL;
	char headers[2048];
	char ua[160];
	DWORD flags;
	DWORD status = 0;
	DWORD slen;
	uint8 buf[16384];
	DWORD got;
	uint8 *body = NULL;
	uint32 cap = 0;
	uint32 n = 0;
	int ok = 0;

	strncpy(ua, r->ua[0] ? r->ua : "Valve/Steam HTTP Client 1.0", sizeof(ua) - 1);
	ua[sizeof(ua) - 1] = '\0';
	Vellum_HttpBuildHeaders(r, headers, (int)sizeof(headers));

	ses = InternetOpenA(ua, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	if (ses == NULL) {
		goto done;
	}
	if (r->timeout_ms) {
		DWORD ms = r->timeout_ms;
		InternetSetOptionA(ses, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
		InternetSetOptionA(ses, INTERNET_OPTION_SEND_TIMEOUT, &ms, sizeof(ms));
		InternetSetOptionA(ses, INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
	}
	flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION |
		INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS;
	if (_strnicmp(r->url, "https://", 8) == 0) {
		flags |= INTERNET_FLAG_SECURE;
	}
	req = InternetOpenUrlA(ses, r->url, headers[0] ? headers : NULL, headers[0] ? (DWORD)-1 : 0, flags, 0);
	if (req == NULL) {
		goto done;
	}
	slen = sizeof(status);
	if (HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &slen, NULL)) {
		ok = 1;
	}
	if (r->method != k_EHTTPMethodHEAD) {
		while (!r->abort) {
			got = 0;
			if (!InternetReadFile(req, buf, sizeof(buf), &got) || got == 0) {
				break;
			}
			if (n + got > VELLUM_HTTP_BODY_MAX) {
				ok = 0;
				status = 0;
				break;
			}
			if (n + got > cap) {
				uint32 ncap = cap ? cap * 2 : 65536;
				uint8 *nb;
				while (ncap < n + got) {
					ncap *= 2;
				}
				nb = (uint8 *)realloc(body, ncap);
				if (nb == NULL) {
					ok = 0;
					status = 0;
					break;
				}
				body = nb;
				cap = ncap;
			}
			memcpy(body + n, buf, got);
			n += got;
			r->body_got = n;
			if (r->body_size) {
				r->progress = (float)n * 100.0f / (float)r->body_size;
			}
		}
	}

done:
	if (r->abort) {
		ok = 0;
		status = 0;
		free(body);
		body = NULL;
		n = 0;
	}
	EnterCriticalSection(&r->lock);
	r->ok = ok != 0;
	r->status = status;
	r->body = body;
	r->body_size = n;
	r->body_got = n;
	r->progress = 100.0f;
	r->timed_out = (!ok && !r->abort) ? 1 : 0;
	r->done = 1;
	LeaveCriticalSection(&r->lock);
	if (req) {
		InternetCloseHandle(req);
	}
	if (ses) {
		InternetCloseHandle(ses);
	}
	return 0;
}
#endif

static HTTPRequestHandle Vellum_HttpCreate(int method, const char *url)
{
	int i;
	VellumHttpReq *r = NULL;
	if (url == NULL || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
		return 0;
	}
	Vellum_HttpLockInit();
	for (i = 0; i < VELLUM_HTTP_MAX; i++) {
		if (!g_http_reqs[i].used) {
			r = &g_http_reqs[i];
			break;
		}
	}
	if (r == NULL) {
		return 0;
	}
	memset(r, 0, sizeof(*r));
#ifdef _WIN32
	InitializeCriticalSection(&r->lock);
	r->lock_ready = 1;
#endif
	r->used = 1;
	r->id = g_http_next++;
	if (g_http_next == 0) {
		g_http_next = 1;
	}
	r->method = method ? method : k_EHTTPMethodGET;
	r->timeout_ms = 30000;
	strncpy(r->url, url, sizeof(r->url) - 1);
	Vellum_Log("HTTP Create id=%u %s", r->id, r->url);
	return r->id;
}

static bool Vellum_HttpSend(HTTPRequestHandle h, SteamAPICall_t *call, int /*stream*/)
{
	VellumHttpReq *r = Vellum_HttpFind(h);
	if (r == NULL || r->sent) {
		return false;
	}
	r->call = g_http_call_next++;
	if (g_http_call_next == 0) {
		g_http_call_next = 1;
	}
	if (call) {
		*call = r->call;
	}
	r->sent = 1;
#ifdef _WIN32
	r->thread = CreateThread(NULL, 0, Vellum_HttpWorker, r, 0, NULL);
	if (r->thread == NULL) {
		r->ok = false;
		r->done = 1;
		return true;
	}
#else
	r->ok = false;
	r->done = 1;
#endif
	Vellum_Log("HTTP Send id=%u call=%u", r->id, (unsigned)r->call);
	return true;
}

static void Vellum_HttpFree(VellumHttpReq *r)
{
#ifdef _WIN32
	r->abort = 1;
	if (r->thread) {
		WaitForSingleObject(r->thread, 8000);
		CloseHandle(r->thread);
		r->thread = NULL;
	}
	if (r->lock_ready) {
		DeleteCriticalSection(&r->lock);
		r->lock_ready = 0;
	}
#endif
	free(r->body);
	memset(r, 0, sizeof(*r));
}

void Vellum_HttpThink()
{
	int i;
	for (i = 0; i < VELLUM_HTTP_MAX; i++) {
		VellumHttpReq *r = &g_http_reqs[i];
		HTTPRequestCompleted_t done;
		SteamAPICallCompleted_t call;
		if (!r->used || !r->sent || r->notified) {
			continue;
		}
#ifdef _WIN32
		if (r->lock_ready) {
			EnterCriticalSection(&r->lock);
		}
#endif
		if (!r->done) {
#ifdef _WIN32
			if (r->lock_ready) {
				LeaveCriticalSection(&r->lock);
			}
#endif
			continue;
		}
#ifdef _WIN32
		if (r->lock_ready) {
			LeaveCriticalSection(&r->lock);
		}
#endif
		memset(&done, 0, sizeof(done));
		done.m_hRequest = r->id;
		done.m_ulContextValue = r->context;
		done.m_bRequestSuccessful = r->ok;
		done.m_eStatusCode = (int)r->status;
		done.m_unBodySize = r->body_size;
		memset(&call, 0, sizeof(call));
		call.m_hAsyncCall = r->call;
		call.m_iCallback = k_iSteamHTTPCallbacks + 1;
		call.m_cubParam = (uint32)sizeof(done);
		Vellum_QueueCallback(1, k_iSteamUtilsCallbacks + 3, &call, (int)sizeof(call));
		r->notified = 1;
		Vellum_Log("HTTP Done id=%u ok=%d status=%u size=%u",
		           r->id, (int)r->ok, r->status, r->body_size);
	}
}

bool Vellum_HttpIsCallCompleted(SteamAPICall_t call, bool *failed)
{
	VellumHttpReq *r = Vellum_HttpFindCall(call);
	if (r == NULL || !r->sent) {
		return false;
	}
	if (!r->done) {
		return false;
	}
	if (failed) {
		*failed = false;
	}
	return true;
}

bool Vellum_HttpCallPending(SteamAPICall_t call)
{
	VellumHttpReq *r = Vellum_HttpFindCall(call);
	return r != NULL && r->sent && !r->done;
}

bool Vellum_HttpGetCallResult(SteamAPICall_t call, void *data, int cub, int expected, bool *failed)
{
	VellumHttpReq *r = Vellum_HttpFindCall(call);
	HTTPRequestCompleted_t done;
	if (r == NULL || !r->done) {
		return false;
	}
	if (expected != 0 && expected != k_iSteamHTTPCallbacks + 1) {
		return false;
	}
	if (data == NULL || cub < (int)sizeof(done)) {
		return false;
	}
	memset(&done, 0, sizeof(done));
	done.m_hRequest = r->id;
	done.m_ulContextValue = r->context;
	done.m_bRequestSuccessful = r->ok;
	done.m_eStatusCode = (int)r->status;
	done.m_unBodySize = r->body_size;
	memcpy(data, &done, sizeof(done));
	if (failed) {
		*failed = false;
	}
	return true;
}

class SteamHTTP {
public:
	virtual HTTPRequestHandle CreateHTTPRequest(int method, const char *url)
	{
		return Vellum_HttpCreate(method, url);
	}
	virtual bool SetHTTPRequestContextValue(HTTPRequestHandle h, uint64 ctx)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL) {
			return false;
		}
		r->context = ctx;
		return true;
	}
	virtual bool SetHTTPRequestNetworkActivityTimeout(HTTPRequestHandle h, uint32 sec)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL) {
			return false;
		}
		r->timeout_ms = sec ? sec * 1000 : 30000;
		return true;
	}
	virtual bool SetHTTPRequestHeaderValue(HTTPRequestHandle h, const char *name, const char *value)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL || name == NULL || value == NULL || r->nhdr >= VELLUM_HTTP_HDR_MAX) {
			return false;
		}
		strncpy(r->hdrs[r->nhdr].name, name, sizeof(r->hdrs[0].name) - 1);
		strncpy(r->hdrs[r->nhdr].value, value, sizeof(r->hdrs[0].value) - 1);
		r->nhdr++;
		return true;
	}
	virtual bool SetHTTPRequestGetOrPostParameter(HTTPRequestHandle, const char *, const char *) { return true; }
	virtual bool SendHTTPRequest(HTTPRequestHandle h, SteamAPICall_t *call)
	{
		return Vellum_HttpSend(h, call, 0);
	}
	virtual bool SendHTTPRequestAndStreamResponse(HTTPRequestHandle h, SteamAPICall_t *call)
	{
		return Vellum_HttpSend(h, call, 1);
	}
	virtual bool DeferHTTPRequest(HTTPRequestHandle) { return true; }
	virtual bool PrioritizeHTTPRequest(HTTPRequestHandle) { return true; }
	virtual bool GetHTTPResponseHeaderSize(HTTPRequestHandle, const char *, uint32 *) { return false; }
	virtual bool GetHTTPResponseHeaderValue(HTTPRequestHandle, const char *, uint8 *, uint32) { return false; }
	virtual bool GetHTTPResponseBodySize(HTTPRequestHandle h, uint32 *size)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL || !r->done || size == NULL) {
			return false;
		}
		*size = r->body_size;
		return true;
	}
	virtual bool GetHTTPResponseBodyData(HTTPRequestHandle h, uint8 *buf, uint32 size)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL || !r->done || buf == NULL || size < r->body_size) {
			return false;
		}
		if (r->body_size && r->body) {
			memcpy(buf, r->body, r->body_size);
		}
		return true;
	}
	virtual bool GetHTTPStreamingResponseBodyData(HTTPRequestHandle h, uint32 off, uint8 *buf, uint32 size)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL || buf == NULL) {
			return false;
		}
		if (off >= r->body_got) {
			return false;
		}
		if (off + size > r->body_got) {
			return false;
		}
		if (r->body) {
			memcpy(buf, r->body + off, size);
		}
		return true;
	}
	virtual bool ReleaseHTTPRequest(HTTPRequestHandle h)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL) {
			return false;
		}
		Vellum_Log("HTTP Release id=%u", h);
		Vellum_HttpFree(r);
		return true;
	}
	virtual bool GetHTTPDownloadProgressPct(HTTPRequestHandle h, float *pct)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL || pct == NULL) {
			return false;
		}
		*pct = r->progress;
		return true;
	}
	virtual bool SetHTTPRequestRawPostBody(HTTPRequestHandle, const char *, uint8 *, uint32) { return false; }
	virtual uint32 CreateCookieContainer(bool) { return 1; }
	virtual bool ReleaseCookieContainer(uint32) { return true; }
	virtual bool SetCookie(uint32, const char *, const char *, const char *) { return true; }
	virtual bool SetHTTPRequestCookieContainer(HTTPRequestHandle, uint32) { return true; }
	virtual bool SetHTTPRequestUserAgentInfo(HTTPRequestHandle h, const char *ua)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL) {
			return false;
		}
		if (ua) {
			strncpy(r->ua, ua, sizeof(r->ua) - 1);
		}
		return true;
	}
	virtual bool SetHTTPRequestRequiresVerifiedCertificate(HTTPRequestHandle, bool) { return true; }
	virtual bool SetHTTPRequestAbsoluteTimeoutMS(HTTPRequestHandle h, uint32 ms)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL) {
			return false;
		}
		r->timeout_ms = ms ? ms : 30000;
		return true;
	}
	virtual bool GetHTTPRequestWasTimedOut(HTTPRequestHandle h, bool *out)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL) {
			return false;
		}
		if (out) {
			*out = r->timed_out != 0;
		}
		return true;
	}
};

static SteamHTTP g_http;

void *Vellum_SteamHTTP()
{
	return &g_http;
}
