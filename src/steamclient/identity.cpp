#include "identity.h"
#include "revemu2013.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

void Vellum_Log(const char *fmt, ...);

#ifdef _WIN32
#define vellum_snprintf _snprintf
#else
#define vellum_snprintf snprintf
#endif

static_assert(sizeof(VellumTicket) == 64, "Vellum ticket must stay 64 bytes");

static VellumIdentity g_id;
static int g_ready;

static void JoinPath(char *out, size_t outSize, const char *dir, const char *file)
{
	size_t n = strlen(dir);
	if (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) {
		vellum_snprintf(out, outSize, "%s%s", dir, file);
	} else {
#ifdef _WIN32
		vellum_snprintf(out, outSize, "%s\\%s", dir, file);
#else
		vellum_snprintf(out, outSize, "%s/%s", dir, file);
#endif
	}
	out[outSize - 1] = '\0';
}

#ifdef _WIN32
static void DirFromThisDll(char *dir, size_t dirSize)
{
	HMODULE mod = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCSTR)&Vellum_InitIdentity, &mod);
	GetModuleFileNameA(mod, dir, (DWORD)dirSize);
	dir[dirSize - 1] = '\0';
	char *slash = strrchr(dir, '\\');
	if (slash != NULL) {
		slash[1] = '\0';
	}
}
#else
static void DirFromThisDll(char *dir, size_t dirSize)
{
	ssize_t n = readlink("/proc/self/exe", dir, dirSize - 1);
	char *slash;

	if (n > 0) {
		dir[n] = '\0';
		slash = strrchr(dir, '/');
		if (slash != NULL) {
			slash[1] = '\0';
			return;
		}
	}
	strncpy(dir, "./", dirSize - 1);
	dir[dirSize - 1] = '\0';
}
#endif

static uint32 ReadAppId(const char *dir)
{
	const char *env = getenv("SteamAppId");
	if (env != NULL && env[0] != '\0') {
		return (uint32)strtoul(env, NULL, 10);
	}
	char path[MAX_PATH];
	JoinPath(path, sizeof(path), dir, "steam_appid.txt");
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return 10;
	}
	char buf[32] = {};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	uint32 id = (uint32)strtoul(buf, NULL, 10);
	return id ? id : 10;
}

static void FillAuthKey(char *key, size_t keySize, unsigned serial)
{
	if (serial == 0) {
		serial = 1;
	}
	vellum_snprintf(key, keySize, "%u", serial);
	key[keySize - 1] = '\0';
}

#ifdef _WIN32
static void FillPersona(char *persona, size_t personaSize)
{
	DWORD n = (DWORD)personaSize;
	if (!GetUserNameA(persona, &n) || persona[0] == '\0') {
		strncpy(persona, "Vellum", personaSize - 1);
		persona[personaSize - 1] = '\0';
	}
}

static void FillIdent(char *ident, size_t identSize, uint16 *ident_len, char *auth_key, size_t authSize)
{
	char computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
	DWORD cn = sizeof(computer);
	if (!GetComputerNameA(computer, &cn) || computer[0] == '\0') {
		strncpy(computer, "PC", sizeof(computer) - 1);
	}

	DWORD serial = 0;
	GetVolumeInformationA("C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0);

	_snprintf(ident, identSize, "%s-%08X", computer, (unsigned)serial);
	ident[identSize - 1] = '\0';
	*ident_len = (uint16)strlen(ident);
	FillAuthKey(auth_key, authSize, (unsigned)serial);
}
#else
static void FillPersona(char *persona, size_t personaSize)
{
	struct passwd *pw = getpwuid(getuid());
	if (pw != NULL && pw->pw_name != NULL && pw->pw_name[0] != '\0') {
		strncpy(persona, pw->pw_name, personaSize - 1);
		persona[personaSize - 1] = '\0';
	} else {
		strncpy(persona, "Vellum", personaSize - 1);
		persona[personaSize - 1] = '\0';
	}
}

static uint32 ReadMachineSerial(void)
{
	FILE *f = fopen("/etc/machine-id", "r");
	char buf[64] = {};
	size_t n;
	size_t i;
	unsigned v = 0;
	int digits = 0;

	if (f == NULL) {
		f = fopen("/var/lib/dbus/machine-id", "r");
	}
	if (f == NULL) {
		return 1;
	}
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	for (i = 0; buf[i] != '\0' && digits < 8; i++) {
		unsigned d;
		char c = buf[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			continue;
		}
		if (c >= '0' && c <= '9') {
			d = (unsigned)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			d = (unsigned)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			d = (unsigned)(c - 'A' + 10);
		} else {
			break;
		}
		v = (v << 4) | d;
		digits++;
	}
	return digits == 8 && v != 0 ? (uint32)v : 1u;
}

static void FillIdent(char *ident, size_t identSize, uint16 *ident_len, char *auth_key, size_t authSize)
{
	char computer[256] = {};
	unsigned serial;
	if (gethostname(computer, sizeof(computer) - 1) != 0 || computer[0] == '\0') {
		strncpy(computer, "PC", sizeof(computer) - 1);
	}
	computer[sizeof(computer) - 1] = '\0';
	/* ident is 48 bytes: hostname + '-' + 8 hex. Cap like Win32 NetBIOS. */
	if (strlen(computer) > 32) {
		computer[32] = '\0';
	}

	serial = (unsigned)ReadMachineSerial();
	snprintf(ident, identSize, "%s-%08X", computer, serial);
	ident[identSize - 1] = '\0';
	*ident_len = (uint16)strlen(ident);
	FillAuthKey(auth_key, authSize, serial);
}
#endif

void Vellum_InitIdentity()
{
	if (g_ready) {
		return;
	}
	memset(&g_id, 0, sizeof(g_id));

	char dir[MAX_PATH];
	DirFromThisDll(dir, sizeof(dir));

	FillPersona(g_id.persona, sizeof(g_id.persona));
	FillIdent(g_id.ident, sizeof(g_id.ident), &g_id.ident_len, g_id.auth_key, sizeof(g_id.auth_key));

#ifdef VELLUM_AUTH_REVEMU2013
	g_id.account_id = RevEmu2013_AccountId(g_id.auth_key);
#else
	g_id.account_id = Vellum_AccountIdFromIdent(g_id.ident, g_id.ident_len);
#endif
	g_id.app_id = ReadAppId(dir);
	g_id.steam_id = CSteamID(g_id.account_id, k_EUniversePublic, k_EAccountTypeIndividual);
	g_ready = 1;
#ifdef VELLUM_AUTH_REVEMU2013
	Vellum_Log("ident kind=revemu2013 persona=%s id=%s authkey=%s account=%u app=%u steamid=%llu",
	           g_id.persona, g_id.ident, g_id.auth_key, (unsigned)g_id.account_id, (unsigned)g_id.app_id,
	           (unsigned long long)g_id.steam_id.ConvertToUint64());
#else
	Vellum_Log("ident kind=vellum persona=%s id=%s account=%u app=%u steamid=%llu",
	           g_id.persona, g_id.ident, (unsigned)g_id.account_id, (unsigned)g_id.app_id,
	           (unsigned long long)g_id.steam_id.ConvertToUint64());
#endif
}

const VellumIdentity &Vellum_GetIdentity()
{
	Vellum_InitIdentity();
	return g_id;
}

int Vellum_WriteAuthBlob(void *blob, int maxBytes)
{
	const VellumIdentity &id = Vellum_GetIdentity();
#ifdef VELLUM_AUTH_REVEMU2013
	return RevEmu2013_WriteTicket(blob, maxBytes, id.auth_key);
#else
	VellumTicket ticket;
	if (blob == NULL || maxBytes < (int)sizeof(VellumTicket)) {
		return 0;
	}
	if (!Vellum_FillTicket(&ticket, id.ident, id.ident_len)) {
		return 0;
	}
	memcpy(blob, &ticket, sizeof(ticket));
	return (int)sizeof(ticket);
#endif
}
