#include "revemu2013.h"

#include <string.h>
#include <time.h>

/* SHA-256 (FIPS 180-4) and Rijndael-256 (32-byte block, 32-byte key) for the
 * RevEmu 2013 ticket. Reunion decrypts with the same Rijndael, not AES-128. */

static uint32_t Rotr32(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

static void Sha256(const unsigned char *data, size_t len, unsigned char out[32])
{
	static const uint32_t k[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
	};
	uint32_t h[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
	};
	unsigned char block[64];
	uint64_t bitlen = (uint64_t)len * 8u;
	size_t i;

	while (len >= 64) {
		memcpy(block, data, 64);
		{
			uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
			int t;
			for (t = 0; t < 16; t++) {
				w[t] = ((uint32_t)block[t * 4] << 24) | ((uint32_t)block[t * 4 + 1] << 16)
				     | ((uint32_t)block[t * 4 + 2] << 8) | (uint32_t)block[t * 4 + 3];
			}
			for (t = 16; t < 64; t++) {
				uint32_t s0 = Rotr32(w[t - 15], 7) ^ Rotr32(w[t - 15], 18) ^ (w[t - 15] >> 3);
				uint32_t s1 = Rotr32(w[t - 2], 17) ^ Rotr32(w[t - 2], 19) ^ (w[t - 2] >> 10);
				w[t] = w[t - 16] + s0 + w[t - 7] + s1;
			}
			a = h[0]; b = h[1]; c = h[2]; d = h[3];
			e = h[4]; f = h[5]; g = h[6]; hh = h[7];
			for (t = 0; t < 64; t++) {
				t1 = hh + (Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25)) + ((e & f) ^ ((~e) & g)) + k[t] + w[t];
				t2 = (Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
				hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
			}
			h[0] += a; h[1] += b; h[2] += c; h[3] += d;
			h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
		}
		data += 64;
		len -= 64;
	}

	memset(block, 0, sizeof(block));
	if (len) {
		memcpy(block, data, len);
	}
	block[len] = 0x80;
	if (len >= 56) {
		{
			uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
			int t;
			for (t = 0; t < 16; t++) {
				w[t] = ((uint32_t)block[t * 4] << 24) | ((uint32_t)block[t * 4 + 1] << 16)
				     | ((uint32_t)block[t * 4 + 2] << 8) | (uint32_t)block[t * 4 + 3];
			}
			for (t = 16; t < 64; t++) {
				uint32_t s0 = Rotr32(w[t - 15], 7) ^ Rotr32(w[t - 15], 18) ^ (w[t - 15] >> 3);
				uint32_t s1 = Rotr32(w[t - 2], 17) ^ Rotr32(w[t - 2], 19) ^ (w[t - 2] >> 10);
				w[t] = w[t - 16] + s0 + w[t - 7] + s1;
			}
			a = h[0]; b = h[1]; c = h[2]; d = h[3];
			e = h[4]; f = h[5]; g = h[6]; hh = h[7];
			for (t = 0; t < 64; t++) {
				t1 = hh + (Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25)) + ((e & f) ^ ((~e) & g)) + k[t] + w[t];
				t2 = (Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
				hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
			}
			h[0] += a; h[1] += b; h[2] += c; h[3] += d;
			h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
		}
		memset(block, 0, sizeof(block));
	}
	block[56] = (unsigned char)(bitlen >> 56);
	block[57] = (unsigned char)(bitlen >> 48);
	block[58] = (unsigned char)(bitlen >> 40);
	block[59] = (unsigned char)(bitlen >> 32);
	block[60] = (unsigned char)(bitlen >> 24);
	block[61] = (unsigned char)(bitlen >> 16);
	block[62] = (unsigned char)(bitlen >> 8);
	block[63] = (unsigned char)bitlen;
	{
		uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
		int t;
		for (t = 0; t < 16; t++) {
			w[t] = ((uint32_t)block[t * 4] << 24) | ((uint32_t)block[t * 4 + 1] << 16)
			     | ((uint32_t)block[t * 4 + 2] << 8) | (uint32_t)block[t * 4 + 3];
		}
		for (t = 16; t < 64; t++) {
			uint32_t s0 = Rotr32(w[t - 15], 7) ^ Rotr32(w[t - 15], 18) ^ (w[t - 15] >> 3);
			uint32_t s1 = Rotr32(w[t - 2], 17) ^ Rotr32(w[t - 2], 19) ^ (w[t - 2] >> 10);
			w[t] = w[t - 16] + s0 + w[t - 7] + s1;
		}
		a = h[0]; b = h[1]; c = h[2]; d = h[3];
		e = h[4]; f = h[5]; g = h[6]; hh = h[7];
		for (t = 0; t < 64; t++) {
			t1 = hh + (Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25)) + ((e & f) ^ ((~e) & g)) + k[t] + w[t];
			t2 = (Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d;
		h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}
	for (i = 0; i < 8; i++) {
		out[i * 4] = (unsigned char)(h[i] >> 24);
		out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
		out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
		out[i * 4 + 3] = (unsigned char)h[i];
	}
}

static const unsigned char kSbox[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
	0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
	0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
	0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
	0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
	0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
	0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
	0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
	0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static unsigned char Gmul(unsigned char a, unsigned char b)
{
	unsigned char p = 0;
	int i;
	for (i = 0; i < 8; i++) {
		if (b & 1) {
			p ^= a;
		}
		{
			unsigned char hi = (unsigned char)(a & 0x80);
			a = (unsigned char)(a << 1);
			if (hi) {
				a ^= 0x1b;
			}
		}
		b = (unsigned char)(b >> 1);
	}
	return p;
}

static void SubBytes(unsigned char *st)
{
	int i;
	for (i = 0; i < 32; i++) {
		st[i] = kSbox[st[i]];
	}
}

static void ShiftRows(unsigned char *st)
{
	unsigned char t[32];
	static const int sh[4] = { 0, 1, 3, 4 };
	int r, c;
	memcpy(t, st, 32);
	for (r = 0; r < 4; r++) {
		for (c = 0; c < 8; c++) {
			int src = (c + sh[r]) % 8;
			st[c * 4 + r] = t[src * 4 + r];
		}
	}
}

static void MixColumns(unsigned char *st)
{
	int c;
	for (c = 0; c < 8; c++) {
		unsigned char *col = st + c * 4;
		unsigned char a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
		col[0] = (unsigned char)(Gmul(a0, 2) ^ Gmul(a1, 3) ^ a2 ^ a3);
		col[1] = (unsigned char)(a0 ^ Gmul(a1, 2) ^ Gmul(a2, 3) ^ a3);
		col[2] = (unsigned char)(a0 ^ a1 ^ Gmul(a2, 2) ^ Gmul(a3, 3));
		col[3] = (unsigned char)(Gmul(a0, 3) ^ a1 ^ a2 ^ Gmul(a3, 2));
	}
}

static void AddRoundKey(unsigned char *st, const unsigned char *rk)
{
	int i;
	for (i = 0; i < 32; i++) {
		st[i] ^= rk[i];
	}
}

static void ExpandKey(const unsigned char key[32], unsigned char rk[15][32])
{
	unsigned char w[480];
	int i;
	static const unsigned char rcon[15] = {
		0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40,
		0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d
	};
	memcpy(w, key, 32);
	for (i = 8; i < 120; i++) {
		unsigned char temp[4];
		memcpy(temp, w + (i - 1) * 4, 4);
		if (i % 8 == 0) {
			unsigned char t = temp[0];
			temp[0] = (unsigned char)(kSbox[temp[1]] ^ rcon[i / 8]);
			temp[1] = kSbox[temp[2]];
			temp[2] = kSbox[temp[3]];
			temp[3] = kSbox[t];
		} else if (i % 8 == 4) {
			temp[0] = kSbox[temp[0]];
			temp[1] = kSbox[temp[1]];
			temp[2] = kSbox[temp[2]];
			temp[3] = kSbox[temp[3]];
		}
		w[i * 4]     = (unsigned char)(w[(i - 8) * 4]     ^ temp[0]);
		w[i * 4 + 1] = (unsigned char)(w[(i - 8) * 4 + 1] ^ temp[1]);
		w[i * 4 + 2] = (unsigned char)(w[(i - 8) * 4 + 2] ^ temp[2]);
		w[i * 4 + 3] = (unsigned char)(w[(i - 8) * 4 + 3] ^ temp[3]);
	}
	for (i = 0; i < 15; i++) {
		memcpy(rk[i], w + i * 32, 32);
	}
}

static void Rijndael256Encrypt(const unsigned char key[32], const unsigned char in[32], unsigned char out[32])
{
	unsigned char rk[15][32];
	unsigned char st[32];
	int r;
	ExpandKey(key, rk);
	memcpy(st, in, 32);
	AddRoundKey(st, rk[0]);
	for (r = 1; r < 14; r++) {
		SubBytes(st);
		ShiftRows(st);
		MixColumns(st);
		AddRoundKey(st, rk[r]);
	}
	SubBytes(st);
	ShiftRows(st);
	AddRoundKey(st, rk[14]);
	memcpy(out, st, 32);
}

uint32_t RevEmu2013_Hash(const char *str)
{
	uint32_t hash = 0x4E67C6A7u;
	if (str == NULL) {
		return hash;
	}
	while (*str) {
		unsigned cc = (unsigned char)*str++;
		hash ^= (hash >> 2) + cc + 32u * hash;
	}
	return hash;
}

uint32_t RevEmu2013_AccountId(const char *str)
{
	return RevEmu2013_Hash(str) << 1;
}

int RevEmu2013_AccountIdFromTicket(const void *blob, int len, uint32_t *account_id)
{
	const unsigned char *p = (const unsigned char *)blob;
	uint32_t version, sig, acc;
	if (blob == NULL || len < 136 || account_id == NULL) {
		return 0;
	}
	memcpy(&version, p, 4);
	memcpy(&sig, p + 8, 4);
	memcpy(&acc, p + 16, 4);
	if (version != 0x53u || sig != 0x00726576u) {
		return 0;
	}
	*account_id = acc;
	return 1;
}

int RevEmu2013_WriteTicket(void *blob, int maxBytes, const char *auth_key)
{
	static const char kAesRand[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
	static const char kAesRev[]  = "_YOU_SERIOUSLY_NEED_TO_GET_LAID_";
	unsigned char plain[32];
	unsigned char enc_data[32];
	unsigned char enc_key[32];
	unsigned char digest[32];
	unsigned char *out;
	uint32_t words[10];
	uint32_t hash;
	uint32_t acc;
	int now;
	size_t n;

	if (blob == NULL || auth_key == NULL || auth_key[0] == '\0' || maxBytes < REVEMU2013_TICKET_SIZE) {
		return 0;
	}

	memset(plain, 0, sizeof(plain));
	n = strlen(auth_key);
	if (n >= sizeof(plain)) {
		n = sizeof(plain) - 1;
	}
	memcpy(plain, auth_key, n);

	hash = RevEmu2013_Hash((const char *)plain);
	acc = hash << 1;
	now = (int)time(NULL);

	Rijndael256Encrypt((const unsigned char *)kAesRand, plain, enc_data);
	Rijndael256Encrypt((const unsigned char *)kAesRev, (const unsigned char *)kAesRand, enc_key);
	Sha256(plain, 32, digest);

	memset(blob, 0, REVEMU2013_TICKET_SIZE);
	out = (unsigned char *)blob;

	words[0] = 0x53u;
	words[1] = hash;
	words[2] = 0x00726576u;
	words[3] = 0;
	words[4] = acc;
	words[5] = 0x01100001u;
	words[6] = (uint32_t)(now + 90123);
	words[7] = (uint32_t)(~now);
	words[8] = (hash * 2u) >> 3;
	words[9] = 0;
	memcpy(out, words, sizeof(words));
	out[27] = (unsigned char)~(out[27] + out[24]);
	memcpy(out + 40, enc_data, 32);
	memcpy(out + 72, enc_key, 32);
	memcpy(out + 104, digest, 32);
	return REVEMU2013_TICKET_SIZE;
}
