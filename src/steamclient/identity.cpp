#include "identity.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static_assert(sizeof(VellumTicket) == 64, "Vellum ticket must stay 64 bytes");

static VellumIdentity g_id;
static int g_ready;

static void JoinPath(char *out, size_t outSize, const char *dir, const char *file)
{
	size_t n = strlen(dir);
	if (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) {
		_snprintf(out, outSize, "%s%s", dir, file);
	} else {
		_snprintf(out, outSize, "%s\\%s", dir, file);
	}
	out[outSize - 1] = '\0';
}

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

void Vellum_InitIdentity()
{
	if (g_ready) {
		return;
	}
	memset(&g_id, 0, sizeof(g_id));

	char dir[MAX_PATH];
	DirFromThisDll(dir, sizeof(dir));
	char iniPath[MAX_PATH];
	JoinPath(iniPath, sizeof(iniPath), dir, "rev.ini");

	GetPrivateProfileStringA("steamclient", "PlayerName", "", g_id.persona, sizeof(g_id.persona), iniPath);
	if (g_id.persona[0] == '\0') {
		DWORD n = sizeof(g_id.persona);
		if (!GetUserNameA(g_id.persona, &n) || g_id.persona[0] == '\0') {
			strncpy(g_id.persona, "Vellum", sizeof(g_id.persona) - 1);
		}
	}

	char computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
	DWORD cn = sizeof(computer);
	if (!GetComputerNameA(computer, &cn) || computer[0] == '\0') {
		strncpy(computer, "PC", sizeof(computer) - 1);
	}

	DWORD serial = 0;
	GetVolumeInformationA("C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0);

	_snprintf(g_id.ident, sizeof(g_id.ident), "%s-%08X", computer, (unsigned)serial);
	g_id.ident[sizeof(g_id.ident) - 1] = '\0';
	g_id.ident_len = (uint16)strlen(g_id.ident);

	g_id.account_id = Vellum_AccountIdFromIdent(g_id.ident, g_id.ident_len);
	g_id.app_id = ReadAppId(dir);
	g_id.steam_id = CSteamID(g_id.account_id, k_EUniversePublic, k_EAccountTypeIndividual);
	g_ready = 1;
}

const VellumIdentity &Vellum_GetIdentity()
{
	Vellum_InitIdentity();
	return g_id;
}

int Vellum_WriteAuthBlob(void *blob, int maxBytes)
{
	if (blob == NULL || maxBytes < (int)sizeof(VellumTicket)) {
		return 0;
	}
	const VellumIdentity &id = Vellum_GetIdentity();
	VellumTicket ticket;
	if (!Vellum_FillTicket(&ticket, id.ident, id.ident_len)) {
		return 0;
	}
	memcpy(blob, &ticket, sizeof(ticket));
	return (int)sizeof(ticket);
}
