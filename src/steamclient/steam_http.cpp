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
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
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
	volatile int done;
	int notified;
	int timed_out;
	volatile int abort;
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
#else
	pthread_t thread;
	pthread_mutex_t lock;
#endif
	int lock_ready;
	int thread_ready;
};

static VellumHttpReq g_http_reqs[VELLUM_HTTP_MAX];
static HTTPRequestHandle g_http_next = 1;
static SteamAPICall_t g_http_call_next = 1;

static void Vellum_HttpReqLock(VellumHttpReq *r)
{
#ifdef _WIN32
	EnterCriticalSection(&r->lock);
#else
	pthread_mutex_lock(&r->lock);
#endif
}

static void Vellum_HttpReqUnlock(VellumHttpReq *r)
{
#ifdef _WIN32
	LeaveCriticalSection(&r->lock);
#else
	pthread_mutex_unlock(&r->lock);
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

static void Vellum_HttpBuildHeaders(VellumHttpReq *r, char *out, int outsz)
{
	int i;
	int n = 0;
	out[0] = '\0';
	for (i = 0; i < r->nhdr && n < outsz - 4; i++) {
#ifdef _WIN32
		n += _snprintf(out + n, (size_t)(outsz - n), "%s: %s\r\n", r->hdrs[i].name, r->hdrs[i].value);
#else
		n += snprintf(out + n, (size_t)(outsz - n), "%s: %s\r\n", r->hdrs[i].name, r->hdrs[i].value);
#endif
		if (n < 0 || n >= outsz) {
			out[outsz - 1] = '\0';
			return;
		}
	}
}

#ifndef _WIN32
typedef void VellumCurl;
typedef int VellumCurlCode;
typedef long long VellumCurlOff;

struct VellumCurlApi {
	void *lib;
	VellumCurlCode (*global_init)(long);
	void (*global_cleanup)(void);
	VellumCurl *(*easy_init)(void);
	VellumCurlCode (*easy_setopt)(VellumCurl *, int, ...);
	VellumCurlCode (*easy_perform)(VellumCurl *);
	VellumCurlCode (*easy_getinfo)(VellumCurl *, int, ...);
	void (*easy_cleanup)(VellumCurl *);
	int ready;
};

struct VellumCurlBuf {
	uint8 *data;
	uint32 n;
	uint32 cap;
	int overflow;
	volatile int *abort_flag;
};

enum {
	Vellum_CURLOPT_URL = 10002,
	Vellum_CURLOPT_WRITEFUNCTION = 20011,
	Vellum_CURLOPT_WRITEDATA = 10001,
	Vellum_CURLOPT_FOLLOWLOCATION = 52,
	Vellum_CURLOPT_TIMEOUT = 13,
	Vellum_CURLOPT_USERAGENT = 10018,
	Vellum_CURLOPT_NOPROGRESS = 43,
	Vellum_CURLOPT_XFERINFOFUNCTION = 20219,
	Vellum_CURLOPT_XFERINFODATA = 10057,
	Vellum_CURLOPT_NOBODY = 44,
	Vellum_CURLOPT_ACCEPT_ENCODING = 10102,
	Vellum_CURLINFO_RESPONSE_CODE = 0x200002,
	Vellum_CURLE_OK = 0,
	Vellum_CURL_GLOBAL_DEFAULT = 3
};

static VellumCurlApi g_curl;

static int Vellum_CurlLoad()
{
	if (g_curl.ready) {
		return 1;
	}
	g_curl.lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
	if (g_curl.lib == NULL) {
		g_curl.lib = dlopen("libcurl.so", RTLD_NOW | RTLD_LOCAL);
	}
	if (g_curl.lib == NULL) {
		Vellum_Log("HTTP curl dlopen fail: %s", dlerror());
		return 0;
	}
	g_curl.global_init = (VellumCurlCode (*)(long))dlsym(g_curl.lib, "curl_global_init");
	g_curl.global_cleanup = (void (*)(void))dlsym(g_curl.lib, "curl_global_cleanup");
	g_curl.easy_init = (VellumCurl *(*)(void))dlsym(g_curl.lib, "curl_easy_init");
	g_curl.easy_setopt = (VellumCurlCode (*)(VellumCurl *, int, ...))dlsym(g_curl.lib, "curl_easy_setopt");
	g_curl.easy_perform = (VellumCurlCode (*)(VellumCurl *))dlsym(g_curl.lib, "curl_easy_perform");
	g_curl.easy_getinfo = (VellumCurlCode (*)(VellumCurl *, int, ...))dlsym(g_curl.lib, "curl_easy_getinfo");
	g_curl.easy_cleanup = (void (*)(VellumCurl *))dlsym(g_curl.lib, "curl_easy_cleanup");
	if (!g_curl.global_init || !g_curl.easy_init || !g_curl.easy_setopt ||
	    !g_curl.easy_perform || !g_curl.easy_getinfo || !g_curl.easy_cleanup) {
		Vellum_Log("HTTP curl dlsym fail");
		dlclose(g_curl.lib);
		memset(&g_curl, 0, sizeof(g_curl));
		return 0;
	}
	g_curl.global_init(Vellum_CURL_GLOBAL_DEFAULT);
	g_curl.ready = 1;
	Vellum_Log("HTTP curl ready");
	return 1;
}

static size_t Vellum_CurlWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	VellumCurlBuf *b = (VellumCurlBuf *)userdata;
	size_t got = size * nmemb;
	uint8 *nb;
	uint32 ncap;
	if (b->abort_flag && *b->abort_flag) {
		return 0;
	}
	if (b->n + (uint32)got > VELLUM_HTTP_BODY_MAX) {
		b->overflow = 1;
		return 0;
	}
	if (b->n + (uint32)got > b->cap) {
		ncap = b->cap ? b->cap * 2 : 65536;
		while (ncap < b->n + (uint32)got) {
			ncap *= 2;
		}
		nb = (uint8 *)realloc(b->data, ncap);
		if (nb == NULL) {
			b->overflow = 1;
			return 0;
		}
		b->data = nb;
		b->cap = ncap;
	}
	memcpy(b->data + b->n, ptr, got);
	b->n += (uint32)got;
	return got;
}

static int Vellum_CurlXfer(void *clientp, VellumCurlOff, VellumCurlOff, VellumCurlOff, VellumCurlOff)
{
	volatile int *abort_flag = (volatile int *)clientp;
	return (abort_flag && *abort_flag) ? 1 : 0;
}

int Vellum_NetHttpGet(const char *url, const char *ua, int timeout_sec, int head_only,
                      uint8 **out_body, uint32 *out_n, uint32 *out_status, volatile int *abort_flag)
{
	VellumCurl *curl;
	VellumCurlBuf buf;
	long status = 0;
	VellumCurlCode rc;
	int ok = 0;

	if (out_body) {
		*out_body = NULL;
	}
	if (out_n) {
		*out_n = 0;
	}
	if (out_status) {
		*out_status = 0;
	}
	if (url == NULL || url[0] == '\0' || !Vellum_CurlLoad()) {
		return 0;
	}
	if (abort_flag && *abort_flag) {
		return 0;
	}
	memset(&buf, 0, sizeof(buf));
	buf.abort_flag = abort_flag;
	curl = g_curl.easy_init();
	if (curl == NULL) {
		return 0;
	}
	g_curl.easy_setopt(curl, Vellum_CURLOPT_URL, url);
	g_curl.easy_setopt(curl, Vellum_CURLOPT_FOLLOWLOCATION, 1L);
	g_curl.easy_setopt(curl, Vellum_CURLOPT_TIMEOUT, (long)(timeout_sec > 0 ? timeout_sec : 20));
	g_curl.easy_setopt(curl, Vellum_CURLOPT_USERAGENT, ua && ua[0] ? ua : "VellumHTTP/1.0");
	g_curl.easy_setopt(curl, Vellum_CURLOPT_ACCEPT_ENCODING, "");
	g_curl.easy_setopt(curl, Vellum_CURLOPT_NOPROGRESS, 1L);
	if (abort_flag) {
		g_curl.easy_setopt(curl, Vellum_CURLOPT_NOPROGRESS, 0L);
		g_curl.easy_setopt(curl, Vellum_CURLOPT_XFERINFOFUNCTION, Vellum_CurlXfer);
		g_curl.easy_setopt(curl, Vellum_CURLOPT_XFERINFODATA, (void *)abort_flag);
	}
	if (head_only) {
		g_curl.easy_setopt(curl, Vellum_CURLOPT_NOBODY, 1L);
	} else {
		g_curl.easy_setopt(curl, Vellum_CURLOPT_WRITEFUNCTION, Vellum_CurlWrite);
		g_curl.easy_setopt(curl, Vellum_CURLOPT_WRITEDATA, &buf);
	}
	rc = g_curl.easy_perform(curl);
	if (rc == Vellum_CURLE_OK && !buf.overflow && !(abort_flag && *abort_flag)) {
		g_curl.easy_getinfo(curl, Vellum_CURLINFO_RESPONSE_CODE, &status);
		ok = 1;
	}
	g_curl.easy_cleanup(curl);
	if (!ok) {
		free(buf.data);
		return 0;
	}
	if (out_body) {
		*out_body = buf.data;
	} else {
		free(buf.data);
	}
	if (out_n) {
		*out_n = buf.n;
	}
	if (out_status) {
		*out_status = (uint32)status;
	}
	return 1;
}
#endif

#ifdef _WIN32
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
#else
static void *Vellum_HttpWorker(void *param)
{
	VellumHttpReq *r = (VellumHttpReq *)param;
	uint8 *body = NULL;
	uint32 n = 0;
	uint32 status = 0;
	int ok = 0;
	char ua[160];
	int timeout_sec;

	strncpy(ua, r->ua[0] ? r->ua : "Valve/Steam HTTP Client 1.0", sizeof(ua) - 1);
	ua[sizeof(ua) - 1] = '\0';
	timeout_sec = (int)((r->timeout_ms ? r->timeout_ms : 30000) + 999) / 1000;
	if (timeout_sec < 5) {
		timeout_sec = 5;
	}
	ok = Vellum_NetHttpGet(r->url, ua, timeout_sec, r->method == k_EHTTPMethodHEAD,
	                       r->method == k_EHTTPMethodHEAD ? NULL : &body, &n, &status, &r->abort);
	if (r->abort) {
		ok = 0;
		status = 0;
		free(body);
		body = NULL;
		n = 0;
	}
	Vellum_HttpReqLock(r);
	r->ok = ok != 0 && status != 0;
	r->status = status;
	r->body = body;
	r->body_size = n;
	r->body_got = n;
	r->progress = 100.0f;
	r->timed_out = (!ok && !r->abort) ? 1 : 0;
	r->done = 1;
	Vellum_HttpReqUnlock(r);
	return NULL;
}
#endif

static HTTPRequestHandle Vellum_HttpCreate(int method, const char *url)
{
	int i;
	VellumHttpReq *r = NULL;
	if (url == NULL || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
		return 0;
	}
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
#else
	if (pthread_mutex_init(&r->lock, NULL) != 0) {
		return 0;
	}
#endif
	r->lock_ready = 1;
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
	if (pthread_create(&r->thread, NULL, Vellum_HttpWorker, r) != 0) {
		r->ok = false;
		r->done = 1;
		Vellum_Log("HTTP Send id=%u call=%u thread fail", r->id, (unsigned)r->call);
		return true;
	}
#endif
	r->thread_ready = 1;
	Vellum_Log("HTTP Send id=%u call=%u", r->id, (unsigned)r->call);
	return true;
}

static void Vellum_HttpFree(VellumHttpReq *r)
{
	r->abort = 1;
	if (r->thread_ready) {
#ifdef _WIN32
		WaitForSingleObject(r->thread, 8000);
		CloseHandle(r->thread);
		r->thread = NULL;
#else
		pthread_join(r->thread, NULL);
#endif
		r->thread_ready = 0;
	}
	if (r->lock_ready) {
#ifdef _WIN32
		DeleteCriticalSection(&r->lock);
#else
		pthread_mutex_destroy(&r->lock);
#endif
		r->lock_ready = 0;
	}
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
		int ready;
		if (!r->used || !r->sent || r->notified) {
			continue;
		}
		if (r->lock_ready) {
			Vellum_HttpReqLock(r);
		}
		ready = r->done;
		if (r->lock_ready) {
			Vellum_HttpReqUnlock(r);
		}
		if (!ready) {
			continue;
		}
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
		r->timeout_ms = sec ? sec * 1000u : 30000u;
		return true;
	}
	virtual bool SetHTTPRequestHeaderValue(HTTPRequestHandle h, const char *name, const char *value)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL || name == NULL || value == NULL || r->nhdr >= VELLUM_HTTP_HDR_MAX) {
			return false;
		}
		strncpy(r->hdrs[r->nhdr].name, name, sizeof(r->hdrs[r->nhdr].name) - 1);
		strncpy(r->hdrs[r->nhdr].value, value, sizeof(r->hdrs[r->nhdr].value) - 1);
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
		if (r == NULL || size == NULL || !r->done) {
			return false;
		}
		*size = r->body_size;
		return true;
	}
	virtual bool GetHTTPResponseBodyData(HTTPRequestHandle h, uint8 *buf, uint32 size)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL || buf == NULL || !r->done || r->body == NULL || size < r->body_size) {
			return false;
		}
		memcpy(buf, r->body, r->body_size);
		return true;
	}
	virtual bool GetHTTPStreamingResponseBodyData(HTTPRequestHandle h, uint32 off, uint8 *buf, uint32 size)
	{
		VellumHttpReq *r = Vellum_HttpFind(h);
		if (r == NULL || buf == NULL || r->body == NULL || off >= r->body_got) {
			return false;
		}
		if (off + size > r->body_got) {
			size = r->body_got - off;
		}
		memcpy(buf, r->body + off, size);
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
