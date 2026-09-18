#include "steam_voice.h"
#include "identity.h"
#include "steam_http.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <opus.h>
#endif

enum {
	kVoiceOk = 0,
	kVoiceNotInitialized = 1,
	kVoiceNotRecording = 2,
	kVoiceNoData = 3,
	kVoiceBufferTooSmall = 4,
	kVoiceDataCorrupted = 5
};

#ifdef _WIN32

#define GS_RATE 24000
#define GS_FRAME 480
#define OPUS_BITRATE 32000
#define CAP_RATE 16000
#define CAP_CHUNK 1600
#define CAP_CHUNKS 4
#define RING_MAX 32768
#define MAX_RX_SLOTS 16
#define MAX_FRAMES_PER_PKT 6
#define MIN_OPUS_SPEECH_BYTES 8
#define VPC_SETSAMPLERATE 11
#define VPC_OPUS_PLC 6

typedef struct RxSlot_s {
	uint64 sid;
	OpusDecoder *dec;
	uint16 seq;
	DWORD lastUsed;
} RxSlot;

static CRITICAL_SECTION g_lock;
static int g_lock_ready;
static int g_recording;
static HWAVEIN g_hwi;
static WAVEHDR g_hdrs[CAP_CHUNKS];
static short g_chunks[CAP_CHUNKS][CAP_CHUNK];
static short g_ring[RING_MAX];
static int g_ring_n;
static short g_in[8192];
static int g_in_n;
static double g_frac;
static short g_pcm24[8192];
static int g_pcm24n;
static short g_last24[8192];
static int g_last24n;
static unsigned char g_pkt[2048];
static uint32 g_pktLen;
static OpusEncoder *g_enc;
static uint16 g_seq;
static uint64 g_sid;
static RxSlot g_rx[MAX_RX_SLOTS];

static unsigned Crc32(const unsigned char *data, unsigned len)
{
	unsigned crc = 0xFFFFFFFFu;
	unsigned i, b, j;
	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (j = 0; j < 8; j++) {
			b = crc & 1u;
			crc >>= 1;
			if (b) {
				crc ^= 0xEDB88320u;
			}
		}
	}
	return crc ^ 0xFFFFFFFFu;
}

static void EnsureLock()
{
	if (!g_lock_ready) {
		InitializeCriticalSection(&g_lock);
		g_lock_ready = 1;
	}
}

static float FrameRms(const short *s, int n)
{
	double acc = 0.0;
	int i;
	if (n <= 0) {
		return 0.0f;
	}
	for (i = 0; i < n; i++) {
		double v = (double)s[i];
		acc += v * v;
	}
	return (float)sqrt(acc / (double)n) / 32768.0f;
}

static int Resample(const short *src, int nSrc, unsigned srcRate, short *dst, int maxDst, unsigned dstRate)
{
	double pos = 0.0;
	double step;
	int out = 0;
	if (src == NULL || dst == NULL || nSrc <= 0 || maxDst <= 0 || srcRate == 0 || dstRate == 0) {
		return 0;
	}
	if (srcRate == dstRate) {
		if (nSrc > maxDst) {
			nSrc = maxDst;
		}
		memcpy(dst, src, (size_t)nSrc * sizeof(short));
		return nSrc;
	}
	step = (double)srcRate / (double)dstRate;
	while (pos + 1.0 < (double)nSrc && out < maxDst) {
		int i = (int)pos;
		double f = pos - (double)i;
		int a = src[i];
		int b = src[i + 1];
		dst[out++] = (short)(a + (int)((b - a) * f));
		pos += step;
	}
	return out;
}

static int EnsureEncoder()
{
	int err = 0;
	if (g_enc != NULL) {
		return 1;
	}
	g_enc = opus_encoder_create(GS_RATE, 1, OPUS_APPLICATION_VOIP, &err);
	if (g_enc == NULL || err != OPUS_OK) {
		Vellum_Log("Voice encoder fail err=%d", err);
		g_enc = NULL;
		return 0;
	}
	opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
	opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	opus_encoder_ctl(g_enc, OPUS_SET_DTX(0));
	opus_encoder_ctl(g_enc, OPUS_SET_INBAND_FEC(1));
	return 1;
}

static RxSlot *GetRxSlot(uint64 sid)
{
	int i;
	int freeIdx = -1;
	int lruIdx = 0;
	DWORD now = GetTickCount();
	int err = 0;
	for (i = 0; i < MAX_RX_SLOTS; i++) {
		if (g_rx[i].dec != NULL && g_rx[i].sid == sid) {
			g_rx[i].lastUsed = now;
			return &g_rx[i];
		}
		if (g_rx[i].dec == NULL && freeIdx < 0) {
			freeIdx = i;
		}
		if (g_rx[i].lastUsed < g_rx[lruIdx].lastUsed) {
			lruIdx = i;
		}
	}
	i = (freeIdx >= 0) ? freeIdx : lruIdx;
	if (g_rx[i].dec != NULL) {
		opus_decoder_destroy(g_rx[i].dec);
		g_rx[i].dec = NULL;
	}
	g_rx[i].dec = opus_decoder_create(GS_RATE, 1, &err);
	if (g_rx[i].dec == NULL || err != OPUS_OK) {
		g_rx[i].dec = NULL;
		return NULL;
	}
	g_rx[i].sid = sid;
	g_rx[i].seq = 0;
	g_rx[i].lastUsed = now;
	return &g_rx[i];
}

static void RingPush(const short *src, int n)
{
	if (src == NULL || n <= 0) {
		return;
	}
	if (n > RING_MAX) {
		src += n - RING_MAX;
		n = RING_MAX;
	}
	if (g_ring_n + n > RING_MAX) {
		int drop = g_ring_n + n - RING_MAX;
		if (drop >= g_ring_n) {
			g_ring_n = 0;
		} else {
			memmove(g_ring, g_ring + drop, (size_t)(g_ring_n - drop) * sizeof(short));
			g_ring_n -= drop;
		}
	}
	memcpy(g_ring + g_ring_n, src, (size_t)n * sizeof(short));
	g_ring_n += n;
}

static int RingPull(short *dst, int maxn)
{
	int n = g_ring_n;
	if (n > maxn) {
		n = maxn;
	}
	if (n <= 0) {
		return 0;
	}
	memcpy(dst, g_ring, (size_t)n * sizeof(short));
	if (n < g_ring_n) {
		memmove(g_ring, g_ring + n, (size_t)(g_ring_n - n) * sizeof(short));
	}
	g_ring_n -= n;
	return n;
}

static void CALLBACK Vellum_WaveInProc(HWAVEIN hwi, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR p2)
{
	WAVEHDR *hdr;
	(void)instance;
	(void)p2;
	if (msg != WIM_DATA) {
		return;
	}
	hdr = (WAVEHDR *)p1;
	EnsureLock();
	EnterCriticalSection(&g_lock);
	if (g_recording && hdr->dwBytesRecorded >= 2) {
		RingPush((const short *)hdr->lpData, (int)(hdr->dwBytesRecorded / 2));
	}
	LeaveCriticalSection(&g_lock);
	if (g_recording && g_hwi == hwi) {
		waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));
	}
}

static void CloseMic()
{
	int i;
	if (g_hwi != NULL) {
		waveInReset(g_hwi);
		for (i = 0; i < CAP_CHUNKS; i++) {
			if (g_hdrs[i].dwFlags & WHDR_PREPARED) {
				waveInUnprepareHeader(g_hwi, &g_hdrs[i], sizeof(WAVEHDR));
			}
		}
		waveInClose(g_hwi);
		g_hwi = NULL;
	}
	memset(g_hdrs, 0, sizeof(g_hdrs));
}

static int OpenMic()
{
	WAVEFORMATEX wfx;
	MMRESULT mm;
	int i;
	memset(&wfx, 0, sizeof(wfx));
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nSamplesPerSec = CAP_RATE;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = 2;
	wfx.nAvgBytesPerSec = CAP_RATE * 2;
	mm = waveInOpen(&g_hwi, WAVE_MAPPER, &wfx, (DWORD_PTR)Vellum_WaveInProc, 0, CALLBACK_FUNCTION);
	if (mm != MMSYSERR_NOERROR) {
		Vellum_Log("Voice waveInOpen mm=%u", (unsigned)mm);
		g_hwi = NULL;
		return 0;
	}
	for (i = 0; i < CAP_CHUNKS; i++) {
		memset(&g_hdrs[i], 0, sizeof(g_hdrs[i]));
		g_hdrs[i].lpData = (LPSTR)g_chunks[i];
		g_hdrs[i].dwBufferLength = (DWORD)(CAP_CHUNK * sizeof(short));
		waveInPrepareHeader(g_hwi, &g_hdrs[i], sizeof(WAVEHDR));
		waveInAddBuffer(g_hwi, &g_hdrs[i], sizeof(WAVEHDR));
	}
	mm = waveInStart(g_hwi);
	if (mm != MMSYSERR_NOERROR) {
		Vellum_Log("Voice waveInStart mm=%u", (unsigned)mm);
		CloseMic();
		return 0;
	}
	return 1;
}

static void AppendCapture(const short *src, int n, unsigned rate)
{
	int idx;
	double f;
	int a, b;
	int used;
	if (src == NULL || n <= 0 || rate == 0) {
		return;
	}
	if (g_in_n + n > (int)(sizeof(g_in) / sizeof(g_in[0]))) {
		g_in_n = 0;
		g_frac = 0.0;
	}
	memcpy(g_in + g_in_n, src, (size_t)n * sizeof(short));
	g_in_n += n;
	while (g_frac + 1.0 < (double)g_in_n && g_pcm24n < (int)(sizeof(g_pcm24) / sizeof(g_pcm24[0]))) {
		idx = (int)g_frac;
		f = g_frac - (double)idx;
		a = g_in[idx];
		b = g_in[idx + 1];
		g_pcm24[g_pcm24n++] = (short)(a + (int)((b - a) * f));
		g_frac += (double)rate / (double)GS_RATE;
	}
	used = (int)g_frac;
	if (used > 0 && used < g_in_n) {
		memmove(g_in, g_in + used, (size_t)(g_in_n - used) * sizeof(short));
		g_in_n -= used;
		g_frac -= (double)used;
	} else if (used >= g_in_n) {
		g_in_n = 0;
		g_frac = 0.0;
	}
}

static uint32 BuildPacket(const unsigned char *payload, unsigned payloadLen)
{
	unsigned off = 0;
	unsigned crc;
	if (payloadLen + 18u > sizeof(g_pkt)) {
		return 0;
	}
	memcpy(g_pkt + off, &g_sid, 8);
	off += 8;
	g_pkt[off++] = VPC_SETSAMPLERATE;
	g_pkt[off++] = (unsigned char)(GS_RATE & 0xFF);
	g_pkt[off++] = (unsigned char)((GS_RATE >> 8) & 0xFF);
	g_pkt[off++] = VPC_OPUS_PLC;
	g_pkt[off++] = (unsigned char)(payloadLen & 0xFF);
	g_pkt[off++] = (unsigned char)((payloadLen >> 8) & 0xFF);
	memcpy(g_pkt + off, payload, payloadLen);
	off += payloadLen;
	crc = Crc32(g_pkt, off);
	memcpy(g_pkt + off, &crc, 4);
	off += 4;
	return off;
}

static void EncodePending()
{
	unsigned char payload[1024];
	unsigned pay = 0;
	unsigned char opusOut[400];
	int nEnc;
	int frames = 0;
	int speech = 0;
	short pulled[4096];
	int nPull;
	if (g_pktLen != 0 || !g_recording) {
		return;
	}
	if (!EnsureEncoder()) {
		return;
	}
	nPull = RingPull(pulled, 4096);
	if (nPull > 0) {
		AppendCapture(pulled, nPull, CAP_RATE);
	}
	g_last24n = 0;
	while (g_pcm24n >= GS_FRAME && frames < MAX_FRAMES_PER_PKT && pay + 16 < sizeof(payload)) {
		if (FrameRms(g_pcm24, GS_FRAME) < 0.008f) {
			memmove(g_pcm24, g_pcm24 + GS_FRAME, (size_t)(g_pcm24n - GS_FRAME) * sizeof(short));
			g_pcm24n -= GS_FRAME;
			continue;
		}
		if (g_last24n + GS_FRAME <= (int)(sizeof(g_last24) / sizeof(g_last24[0]))) {
			memcpy(g_last24 + g_last24n, g_pcm24, GS_FRAME * sizeof(short));
			g_last24n += GS_FRAME;
		}
		nEnc = opus_encode(g_enc, g_pcm24, GS_FRAME, opusOut, (int)sizeof(opusOut));
		memmove(g_pcm24, g_pcm24 + GS_FRAME, (size_t)(g_pcm24n - GS_FRAME) * sizeof(short));
		g_pcm24n -= GS_FRAME;
		if (nEnc < MIN_OPUS_SPEECH_BYTES) {
			continue;
		}
		if (pay + 4 + (unsigned)nEnc > sizeof(payload)) {
			break;
		}
		payload[pay] = (unsigned char)(nEnc & 0xFF);
		payload[pay + 1] = (unsigned char)((nEnc >> 8) & 0xFF);
		payload[pay + 2] = (unsigned char)(g_seq & 0xFF);
		payload[pay + 3] = (unsigned char)((g_seq >> 8) & 0xFF);
		g_seq++;
		memcpy(payload + pay + 4, opusOut, (size_t)nEnc);
		pay += 4 + (unsigned)nEnc;
		frames++;
		speech++;
	}
	if (speech == 0 || pay == 0) {
		g_last24n = 0;
		return;
	}
	g_pktLen = BuildPacket(payload, pay);
}

static int LooksLikeSteamVoice(const unsigned char *p, unsigned n)
{
	unsigned crc;
	unsigned got;
	unsigned pay;
	if (p == NULL || n < 18) {
		return 0;
	}
	if (p[8] != VPC_SETSAMPLERATE) {
		return 0;
	}
	if (p[11] != VPC_OPUS_PLC && p[11] != 4) {
		return 0;
	}
	pay = (unsigned)p[12] | ((unsigned)p[13] << 8);
	if (14u + pay + 4u != n) {
		return 0;
	}
	crc = Crc32(p, n - 4);
	memcpy(&got, p + n - 4, 4);
	return crc == got;
}

static int DecodeSteamPayload(const unsigned char *comp, unsigned compBytes, short *pcm24, int maxSamples)
{
	unsigned off;
	unsigned payLen;
	unsigned end;
	int samples = 0;
	uint64 sid;
	RxSlot *slot;
	if (!LooksLikeSteamVoice(comp, compBytes)) {
		return -1;
	}
	memcpy(&sid, comp, 8);
	slot = GetRxSlot(sid);
	if (slot == NULL || slot->dec == NULL) {
		return -1;
	}
	payLen = (unsigned)comp[12] | ((unsigned)comp[13] << 8);
	off = 14;
	end = 14 + payLen;
	while (off + 4 <= end && samples + GS_FRAME <= maxSamples) {
		unsigned frameBytes = (unsigned)comp[off] | ((unsigned)comp[off + 1] << 8);
		unsigned seq = (unsigned)comp[off + 2] | ((unsigned)comp[off + 3] << 8);
		int got;
		off += 4;
		if (frameBytes == 0xFFFFu) {
			opus_decoder_ctl(slot->dec, OPUS_RESET_STATE);
			slot->seq = 0;
			break;
		}
		if (frameBytes == 0 || off + frameBytes > end) {
			break;
		}
		if (seq != slot->seq && slot->seq != 0) {
			int loss = (int)(seq - slot->seq);
			if (loss > 0 && loss < 10) {
				int i;
				for (i = 0; i < loss && samples + GS_FRAME <= maxSamples; i++) {
					got = opus_decode(slot->dec, NULL, 0, pcm24 + samples, GS_FRAME, 0);
					if (got > 0) {
						samples += got;
					}
				}
			}
		}
		slot->seq = (uint16)(seq + 1);
		got = opus_decode(slot->dec, (const unsigned char *)comp + off, (int)frameBytes, pcm24 + samples, GS_FRAME, 0);
		off += frameBytes;
		if (got > 0) {
			samples += got;
		}
	}
	return samples;
}

void Vellum_VoiceStart()
{
	int need_mic;
	EnsureLock();
	EnterCriticalSection(&g_lock);
	g_sid = Vellum_GetIdentity().steam_id.ConvertToUint64();
	g_recording = 1;
	g_ring_n = 0;
	g_in_n = 0;
	g_frac = 0.0;
	g_pcm24n = 0;
	g_pktLen = 0;
	g_seq = 0;
	g_last24n = 0;
	if (g_enc != NULL) {
		opus_encoder_ctl(g_enc, OPUS_RESET_STATE);
	}
	EnsureEncoder();
	need_mic = (g_hwi == NULL);
	LeaveCriticalSection(&g_lock);
	if (need_mic && !OpenMic()) {
		EnterCriticalSection(&g_lock);
		g_recording = 0;
		LeaveCriticalSection(&g_lock);
		return;
	}
	Vellum_Log("Voice start sid=%llu", (unsigned long long)g_sid);
}

void Vellum_VoiceStop()
{
	EnsureLock();
	EnterCriticalSection(&g_lock);
	g_recording = 0;
	g_pktLen = 0;
	g_ring_n = 0;
	g_pcm24n = 0;
	g_in_n = 0;
	if (g_enc != NULL) {
		opus_encoder_ctl(g_enc, OPUS_RESET_STATE);
	}
	LeaveCriticalSection(&g_lock);
	CloseMic();
	Vellum_Log("Voice stop");
}

int Vellum_VoiceAvailable(uint32 *pcbCompressed, uint32 *pcbUncompressed, uint32 wantRate)
{
	uint32 comp = 0;
	uint32 uncomp = 0;
	EnsureLock();
	EnterCriticalSection(&g_lock);
	if (!g_recording) {
		LeaveCriticalSection(&g_lock);
		if (pcbCompressed) *pcbCompressed = 0;
		if (pcbUncompressed) *pcbUncompressed = 0;
		return kVoiceNotRecording;
	}
	EncodePending();
	comp = g_pktLen;
	if (g_last24n > 0) {
		unsigned rate = wantRate ? wantRate : 11025;
		uncomp = (uint32)((g_last24n * (int)rate / GS_RATE) * (int)sizeof(short));
		if (uncomp < 2) {
			uncomp = (uint32)(g_last24n * (int)sizeof(short));
		}
	}
	LeaveCriticalSection(&g_lock);
	if (pcbCompressed) *pcbCompressed = comp;
	if (pcbUncompressed) *pcbUncompressed = uncomp;
	return comp ? kVoiceOk : kVoiceNoData;
}

int Vellum_VoiceGet(bool wantCompressed, void *dst, uint32 dstBytes, uint32 *wrote,
                    bool wantUncompressed, void *udst, uint32 udstBytes, uint32 *uwrote, uint32 wantRate)
{
	int rc = kVoiceNoData;
	if (wrote) *wrote = 0;
	if (uwrote) *uwrote = 0;
	EnsureLock();
	EnterCriticalSection(&g_lock);
	if (!g_recording) {
		LeaveCriticalSection(&g_lock);
		return kVoiceNotRecording;
	}
	EncodePending();
	if (wantCompressed) {
		if (g_pktLen == 0) {
			LeaveCriticalSection(&g_lock);
			return kVoiceNoData;
		}
		if (dst == NULL || dstBytes < g_pktLen) {
			LeaveCriticalSection(&g_lock);
			return kVoiceBufferTooSmall;
		}
		memcpy(dst, g_pkt, g_pktLen);
		if (wrote) *wrote = g_pktLen;
		g_pktLen = 0;
		rc = kVoiceOk;
	}
	if (wantUncompressed && g_last24n > 0 && udst != NULL) {
		unsigned rate = wantRate ? wantRate : 11025;
		int n = Resample(g_last24, g_last24n, GS_RATE, (short *)udst, (int)(udstBytes / 2), rate);
		if (uwrote) *uwrote = (uint32)n * 2u;
		if (n > 0) {
			rc = kVoiceOk;
		}
	}
	LeaveCriticalSection(&g_lock);
	return rc;
}

int Vellum_VoiceDecompress(const void *comp, uint32 compBytes, void *dst, uint32 dstBytes,
                           uint32 *wrote, uint32 wantRate)
{
	short pcm24[8192];
	int n24;
	int nOut;
	if (wrote) *wrote = 0;
	if (comp == NULL || dst == NULL || dstBytes < 2) {
		return kVoiceDataCorrupted;
	}
	EnsureLock();
	EnterCriticalSection(&g_lock);
	n24 = DecodeSteamPayload((const unsigned char *)comp, compBytes, pcm24, 8192);
	LeaveCriticalSection(&g_lock);
	if (n24 < 0) {
		return kVoiceDataCorrupted;
	}
	if (n24 == 0) {
		return kVoiceOk;
	}
	if (wantRate == 0) {
		wantRate = 11025;
	}
	nOut = Resample(pcm24, n24, GS_RATE, (short *)dst, (int)(dstBytes / 2), wantRate);
	if (wrote) *wrote = (uint32)nOut * 2u;
	return kVoiceOk;
}

uint32 Vellum_VoiceOptimalRate()
{
	return 11025;
}

#else

void Vellum_VoiceStart() {}
void Vellum_VoiceStop() {}

int Vellum_VoiceAvailable(uint32 *pcbCompressed, uint32 *pcbUncompressed, uint32)
{
	if (pcbCompressed) *pcbCompressed = 0;
	if (pcbUncompressed) *pcbUncompressed = 0;
	return kVoiceNotRecording;
}

int Vellum_VoiceGet(bool, void *, uint32, uint32 *wrote, bool, void *, uint32, uint32 *uwrote, uint32)
{
	if (wrote) *wrote = 0;
	if (uwrote) *uwrote = 0;
	return kVoiceNotRecording;
}

int Vellum_VoiceDecompress(const void *, uint32, void *, uint32, uint32 *wrote, uint32)
{
	if (wrote) *wrote = 0;
	return kVoiceNotInitialized;
}

uint32 Vellum_VoiceOptimalRate()
{
	return 11025;
}

#endif
