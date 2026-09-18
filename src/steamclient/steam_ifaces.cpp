#include "types.h"
#include "identity.h"
#include "exports.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#endif

/* Thin Steamworks facades for GoldSrc steam_api.
 * SteamClient012 is the 8684-era layout; SteamClient020 matches the Oct 2024
 * (build 10211) libsteam_api. Methods the engine needs are real; the rest are
 * no-ops with the correct vtable slots so SteamAPI_Init can obtain every iface. */

static void Vellum_Log(const char *fmt, ...)
{
	char dir[MAX_PATH];
	char path[MAX_PATH];
	char line[1024];
	va_list ap;
	FILE *f;
#ifdef _WIN32
	HMODULE mod = NULL;
	char *slash;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCSTR)&Vellum_Log, &mod);
	GetModuleFileNameA(mod, dir, MAX_PATH);
	dir[MAX_PATH - 1] = '\0';
	slash = strrchr(dir, '\\');
	if (slash != NULL) {
		slash[1] = '\0';
	}
	_snprintf(path, sizeof(path), "%svellum.log", dir);
#else
	strncpy(path, "vellum.log", sizeof(path) - 1);
	path[sizeof(path) - 1] = '\0';
#endif
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	line[sizeof(line) - 1] = '\0';
	f = fopen(path, "a");
	if (f == NULL) {
		return;
	}
	fputs(line, f);
	fputc('\n', f);
	fclose(f);
}

#ifdef _WIN32
static LONG CALLBACK Vellum_Veh(EXCEPTION_POINTERS *ep)
{
	MEMORY_BASIC_INFORMATION mbi;
	char mod[MAX_PATH];
	if (ep == NULL || ep->ExceptionRecord == NULL) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	mod[0] = '\0';
	if (VirtualQuery(ep->ExceptionRecord->ExceptionAddress, &mbi, sizeof(mbi)) != 0) {
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod, MAX_PATH);
	}
	Vellum_Log("CRASH code=%08X addr=%p acc=%p module=%s",
	           (unsigned)ep->ExceptionRecord->ExceptionCode,
	           ep->ExceptionRecord->ExceptionAddress,
	           ep->ExceptionRecord->NumberParameters >= 2 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL,
	           mod);
	return EXCEPTION_CONTINUE_SEARCH;
}

static void Vellum_InstallCrashLog(void)
{
	static int once;
	if (!once) {
		once = 1;
		AddVectoredExceptionHandler(1, Vellum_Veh);
	}
}
#else
static void Vellum_InstallCrashLog(void) {}
#endif

static void Vellum_QueueCallback(HSteamUser user, int id, const void *data, int size);
static int Vellum_FillAuthTicket(void *pTicket, int cbMaxTicket, uint32 *pcbTicket);
static void *Vellum_PickUser(const char *ver);
static void *Vellum_PickFriends(const char *ver);
static void Vellum_FlushListCallbacks();

class SteamUser {
public:
	virtual HSteamUser GetHSteamUser() { return 1; }
	virtual bool BLoggedOn() { return true; }
	virtual CSteamID GetSteamID() { return Vellum_GetIdentity().steam_id; }
	virtual int InitiateGameConnection(void *pAuthBlob, int cbMaxAuthBlob, CSteamID, uint32 ip, uint16 port, bool)
	{
		int n;
		Vellum_Log("InitiateGameConnection max=%d ip=%u port=%u", cbMaxAuthBlob, ip, (unsigned)port);
		n = Vellum_WriteAuthBlob(pAuthBlob, cbMaxAuthBlob);
		Vellum_Log("InitiateGameConnection wrote=%d", n);
		return n;
	}
	virtual void TerminateGameConnection(uint32, uint16) {}
	virtual void TrackAppUsageEvent(CGameID, int, const char *) {}
	virtual bool GetUserDataFolder(char *pchBuffer, int cubBuffer)
	{
		if (pchBuffer == NULL || cubBuffer <= 0) {
			return false;
		}
		strncpy(pchBuffer, ".", (size_t)cubBuffer - 1);
		pchBuffer[cubBuffer - 1] = '\0';
		return true;
	}
	virtual void StartVoiceRecording() {}
	virtual void StopVoiceRecording() {}
	virtual int GetAvailableVoice(uint32 *pcbCompressed, uint32 *pcbUncompressed, uint32)
	{
		if (pcbCompressed) *pcbCompressed = 0;
		if (pcbUncompressed) *pcbUncompressed = 0;
		return 1; /* k_EVoiceResultNotRecording */
	}
	virtual int GetVoice(bool, void *, uint32, uint32 *, bool, void *, uint32, uint32 *, uint32) { return 1; }
	virtual int DecompressVoice(const void *, uint32, void *, uint32, uint32 *, uint32) { return 1; }
	virtual uint32 GetVoiceOptimalSampleRate() { return 11025; }
	virtual HAuthTicket GetAuthSessionTicket(void *pTicket, int cbMaxTicket, uint32 *pcbTicket)
	{
		Vellum_Log("GetAuthSessionTicket max=%d", cbMaxTicket);
		return Vellum_FillAuthTicket(pTicket, cbMaxTicket, pcbTicket) ? 1 : 0;
	}
	virtual int BeginAuthSession(const void *, int, CSteamID) { return 0; }
	virtual void EndAuthSession(CSteamID) {}
	virtual void CancelAuthTicket(HAuthTicket) {}
	virtual int UserHasLicenseForApp(CSteamID, AppId_t) { return 0; }
	virtual bool BIsBehindNAT() { return false; }
	virtual void AdvertiseGame(CSteamID, uint32 ip, uint16 port)
	{
		Vellum_Log("AdvertiseGame ip=%u port=%u", ip, (unsigned)port);
	}
	virtual SteamAPICall_t RequestEncryptedAppTicket(void *, int) { return 0; }
	virtual bool GetEncryptedAppTicket(void *, int, uint32 *) { return false; }
	virtual int GetGameBadgeLevel(int, bool) { return 0; }
	virtual int GetPlayerSteamLevel() { return 1; }
};

/* SteamUser023: GetAuthTicketForWebApi sits after GetAuthSessionTicket, so this
 * cannot share a vtable with the 8684 SteamUser above. */
class SteamUser023 {
public:
	virtual HSteamUser GetHSteamUser() { return 1; }
	virtual bool BLoggedOn() { return true; }
	virtual CSteamID GetSteamID() { return Vellum_GetIdentity().steam_id; }
	virtual int InitiateGameConnection_DEPRECATED(void *pAuthBlob, int cbMaxAuthBlob, CSteamID, uint32 ip, uint16 port, bool)
	{
		int n;
		Vellum_Log("InitiateGameConnection_DEPRECATED max=%d ip=%u port=%u", cbMaxAuthBlob, ip, (unsigned)port);
		n = Vellum_WriteAuthBlob(pAuthBlob, cbMaxAuthBlob);
		Vellum_Log("InitiateGameConnection_DEPRECATED wrote=%d", n);
		return n;
	}
	virtual void TerminateGameConnection_DEPRECATED(uint32, uint16) {}
	virtual void TrackAppUsageEvent(CGameID, int, const char *) {}
	virtual bool GetUserDataFolder(char *pchBuffer, int cubBuffer)
	{
		if (pchBuffer == NULL || cubBuffer <= 0) {
			return false;
		}
		strncpy(pchBuffer, ".", (size_t)cubBuffer - 1);
		pchBuffer[cubBuffer - 1] = '\0';
		return true;
	}
	virtual void StartVoiceRecording() {}
	virtual void StopVoiceRecording() {}
	virtual int GetAvailableVoice(uint32 *pcbCompressed, uint32 *pcbUncompressed, uint32)
	{
		if (pcbCompressed) *pcbCompressed = 0;
		if (pcbUncompressed) *pcbUncompressed = 0;
		return 1;
	}
	virtual int GetVoice(bool, void *, uint32, uint32 *, bool, void *, uint32, uint32 *, uint32) { return 1; }
	virtual int DecompressVoice(const void *, uint32, void *, uint32, uint32 *, uint32) { return 1; }
	virtual uint32 GetVoiceOptimalSampleRate() { return 11025; }
	virtual HAuthTicket GetAuthSessionTicket(void *pTicket, int cbMaxTicket, uint32 *pcbTicket, const void *)
	{
		Vellum_Log("GetAuthSessionTicket021 max=%d", cbMaxTicket);
		return Vellum_FillAuthTicket(pTicket, cbMaxTicket, pcbTicket) ? 1 : 0;
	}
	virtual HAuthTicket GetAuthTicketForWebApi(const char *) { return 0; }
	virtual int BeginAuthSession(const void *, int, CSteamID) { return 0; }
	virtual void EndAuthSession(CSteamID) {}
	virtual void CancelAuthTicket(HAuthTicket) {}
	virtual int UserHasLicenseForApp(CSteamID, AppId_t) { return 0; }
	virtual bool BIsBehindNAT() { return false; }
	virtual void AdvertiseGame(CSteamID, uint32 ip, uint16 port)
	{
		Vellum_Log("AdvertiseGame021 ip=%u port=%u", ip, (unsigned)port);
	}
	virtual SteamAPICall_t RequestEncryptedAppTicket(void *, int) { return 0; }
	virtual bool GetEncryptedAppTicket(void *, int, uint32 *) { return false; }
	virtual int GetGameBadgeLevel(int, bool) { return 0; }
	virtual int GetPlayerSteamLevel() { return 1; }
	virtual SteamAPICall_t RequestStoreAuthURL(const char *) { return 0; }
	virtual bool BIsPhoneVerified() { return false; }
	virtual bool BIsTwoFactorEnabled() { return false; }
	virtual bool BIsPhoneIdentifying() { return false; }
	virtual bool BIsPhoneRequiringVerification() { return false; }
	virtual SteamAPICall_t GetMarketEligibility() { return 0; }
	virtual SteamAPICall_t GetDurationControl() { return 0; }
	virtual bool BSetDurationControlOnlineState(int) { return false; }
};

class SteamFriends {
public:
	virtual const char *GetPersonaName() { return Vellum_GetIdentity().persona; }
	virtual SteamAPICall_t SetPersonaName(const char *) { return 0; }
	virtual int GetPersonaState() { return 1; } /* Online */
	virtual int GetFriendCount(int) { return 0; }
	virtual CSteamID GetFriendByIndex(int, int) { return CSteamID(); }
	virtual int GetFriendRelationship(CSteamID) { return 0; }
	virtual int GetFriendPersonaState(CSteamID) { return 0; }
	virtual const char *GetFriendPersonaName(CSteamID) { return ""; }
	virtual bool GetFriendGamePlayed(CSteamID, FriendGameInfo_t *) { return false; }
	virtual const char *GetFriendPersonaNameHistory(CSteamID, int) { return ""; }
	virtual const char *GetPlayerNickname(CSteamID) { return ""; }
	virtual bool HasFriend(CSteamID, int) { return false; }
	virtual int GetClanCount() { return 0; }
	virtual CSteamID GetClanByIndex(int) { return CSteamID(); }
	virtual const char *GetClanName(CSteamID) { return ""; }
	virtual const char *GetClanTag(CSteamID) { return ""; }
	virtual bool GetClanActivityCounts(CSteamID, int *, int *, int *) { return false; }
	virtual SteamAPICall_t DownloadClanActivityCounts(CSteamID[], int) { return 0; }
	virtual int GetFriendCountFromSource(CSteamID) { return 0; }
	virtual CSteamID GetFriendFromSourceByIndex(CSteamID, int) { return CSteamID(); }
	virtual bool IsUserInSource(CSteamID, CSteamID) { return false; }
	virtual void SetInGameVoiceSpeaking(CSteamID, bool) {}
	virtual void ActivateGameOverlay(const char *) {}
	virtual void ActivateGameOverlayToUser(const char *, CSteamID) {}
	virtual void ActivateGameOverlayToWebPage(const char *) {}
	virtual void ActivateGameOverlayToStore(AppId_t, int) {}
	virtual void SetPlayedWith(CSteamID) {}
	virtual void ActivateGameOverlayInviteDialog(CSteamID) {}
	virtual int GetSmallFriendAvatar(CSteamID) { return 0; }
	virtual int GetMediumFriendAvatar(CSteamID) { return 0; }
	virtual int GetLargeFriendAvatar(CSteamID) { return 0; }
	virtual bool RequestUserInformation(CSteamID, bool) { return false; }
	virtual SteamAPICall_t RequestClanOfficerList(CSteamID) { return 0; }
	virtual CSteamID GetClanOwner(CSteamID) { return CSteamID(); }
	virtual int GetClanOfficerCount(CSteamID) { return 0; }
	virtual CSteamID GetClanOfficerByIndex(CSteamID, int) { return CSteamID(); }
	virtual uint32 GetUserRestrictions() { return 0; }
	virtual bool SetRichPresence(const char *, const char *) { return true; }
	virtual void ClearRichPresence() {}
	virtual const char *GetFriendRichPresence(CSteamID, const char *) { return ""; }
	virtual int GetFriendRichPresenceKeyCount(CSteamID) { return 0; }
	virtual const char *GetFriendRichPresenceKeyByIndex(CSteamID, int) { return ""; }
	virtual void RequestFriendRichPresence(CSteamID) {}
	virtual bool InviteUserToGame(CSteamID, const char *) { return false; }
	virtual int GetCoplayFriendCount() { return 0; }
	virtual CSteamID GetCoplayFriend(int) { return CSteamID(); }
	virtual int GetFriendCoplayTime(CSteamID) { return 0; }
	virtual AppId_t GetFriendCoplayGame(CSteamID) { return 0; }
	virtual SteamAPICall_t JoinClanChatRoom(CSteamID) { return 0; }
	virtual bool LeaveClanChatRoom(CSteamID) { return false; }
	virtual int GetClanChatMemberCount(CSteamID) { return 0; }
	virtual CSteamID GetChatMemberByIndex(CSteamID, int) { return CSteamID(); }
	virtual bool SendClanChatMessage(CSteamID, const char *) { return false; }
	virtual int GetClanChatMessage(CSteamID, int, void *, int, int *, CSteamID *) { return 0; }
	virtual bool IsClanChatAdmin(CSteamID, CSteamID) { return false; }
	virtual bool IsClanChatWindowOpenInSteam(CSteamID) { return false; }
	virtual bool OpenClanChatWindowInSteam(CSteamID) { return false; }
	virtual bool CloseClanChatWindowInSteam(CSteamID) { return false; }
	virtual bool SetListenForFriendsMessages(bool) { return false; }
	virtual bool ReplyToFriendMessage(CSteamID, const char *) { return false; }
	virtual int GetFriendMessage(CSteamID, int, void *, int, int *) { return 0; }
	virtual SteamAPICall_t GetFollowerCount(CSteamID) { return 0; }
	virtual SteamAPICall_t IsFollowing(CSteamID) { return 0; }
	virtual SteamAPICall_t EnumerateFollowingList(uint32) { return 0; }
};

class SteamFriends017 {
public:
	virtual const char *GetPersonaName() { return Vellum_GetIdentity().persona; }
	virtual SteamAPICall_t SetPersonaName(const char *) { return 0; }
	virtual int GetPersonaState() { return 1; }
	virtual int GetFriendCount(int) { return 0; }
	virtual CSteamID GetFriendByIndex(int, int) { return CSteamID(); }
	virtual int GetFriendRelationship(CSteamID) { return 0; }
	virtual int GetFriendPersonaState(CSteamID) { return 0; }
	virtual const char *GetFriendPersonaName(CSteamID) { return ""; }
	virtual bool GetFriendGamePlayed(CSteamID, FriendGameInfo_t *) { return false; }
	virtual const char *GetFriendPersonaNameHistory(CSteamID, int) { return ""; }
	virtual int GetFriendSteamLevel(CSteamID) { return 1; }
	virtual const char *GetPlayerNickname(CSteamID) { return ""; }
	virtual int GetFriendsGroupCount() { return 0; }
	virtual int16 GetFriendsGroupIDByIndex(int) { return -1; }
	virtual const char *GetFriendsGroupName(int16) { return ""; }
	virtual int GetFriendsGroupMembersCount(int16) { return 0; }
	virtual void GetFriendsGroupMembersList(int16, CSteamID *, int) {}
	virtual bool HasFriend(CSteamID, int) { return false; }
	virtual int GetClanCount() { return 0; }
	virtual CSteamID GetClanByIndex(int) { return CSteamID(); }
	virtual const char *GetClanName(CSteamID) { return ""; }
	virtual const char *GetClanTag(CSteamID) { return ""; }
	virtual bool GetClanActivityCounts(CSteamID, int *, int *, int *) { return false; }
	virtual SteamAPICall_t DownloadClanActivityCounts(CSteamID[], int) { return 0; }
	virtual int GetFriendCountFromSource(CSteamID) { return 0; }
	virtual CSteamID GetFriendFromSourceByIndex(CSteamID, int) { return CSteamID(); }
	virtual bool IsUserInSource(CSteamID, CSteamID) { return false; }
	virtual void SetInGameVoiceSpeaking(CSteamID, bool) {}
	virtual void ActivateGameOverlay(const char *) {}
	virtual void ActivateGameOverlayToUser(const char *, CSteamID) {}
	virtual void ActivateGameOverlayToWebPage(const char *, int) {}
	virtual void ActivateGameOverlayToStore(AppId_t, int) {}
	virtual void SetPlayedWith(CSteamID) {}
	virtual void ActivateGameOverlayInviteDialog(CSteamID) {}
	virtual int GetSmallFriendAvatar(CSteamID) { return 0; }
	virtual int GetMediumFriendAvatar(CSteamID) { return 0; }
	virtual int GetLargeFriendAvatar(CSteamID) { return 0; }
	virtual bool RequestUserInformation(CSteamID, bool) { return false; }
	virtual SteamAPICall_t RequestClanOfficerList(CSteamID) { return 0; }
	virtual CSteamID GetClanOwner(CSteamID) { return CSteamID(); }
	virtual int GetClanOfficerCount(CSteamID) { return 0; }
	virtual CSteamID GetClanOfficerByIndex(CSteamID, int) { return CSteamID(); }
	virtual uint32 GetUserRestrictions() { return 0; }
	virtual bool SetRichPresence(const char *, const char *) { return true; }
	virtual void ClearRichPresence() {}
	virtual const char *GetFriendRichPresence(CSteamID, const char *) { return ""; }
	virtual int GetFriendRichPresenceKeyCount(CSteamID) { return 0; }
	virtual const char *GetFriendRichPresenceKeyByIndex(CSteamID, int) { return ""; }
	virtual void RequestFriendRichPresence(CSteamID) {}
	virtual bool InviteUserToGame(CSteamID, const char *) { return false; }
	virtual int GetCoplayFriendCount() { return 0; }
	virtual CSteamID GetCoplayFriend(int) { return CSteamID(); }
	virtual int GetFriendCoplayTime(CSteamID) { return 0; }
	virtual AppId_t GetFriendCoplayGame(CSteamID) { return 0; }
	virtual SteamAPICall_t JoinClanChatRoom(CSteamID) { return 0; }
	virtual bool LeaveClanChatRoom(CSteamID) { return false; }
	virtual int GetClanChatMemberCount(CSteamID) { return 0; }
	virtual CSteamID GetChatMemberByIndex(CSteamID, int) { return CSteamID(); }
	virtual bool SendClanChatMessage(CSteamID, const char *) { return false; }
	virtual int GetClanChatMessage(CSteamID, int, void *, int, int *, CSteamID *) { return 0; }
	virtual bool IsClanChatAdmin(CSteamID, CSteamID) { return false; }
	virtual bool IsClanChatWindowOpenInSteam(CSteamID) { return false; }
	virtual bool OpenClanChatWindowInSteam(CSteamID) { return false; }
	virtual bool CloseClanChatWindowInSteam(CSteamID) { return false; }
	virtual bool SetListenForFriendsMessages(bool) { return false; }
	virtual bool ReplyToFriendMessage(CSteamID, const char *) { return false; }
	virtual int GetFriendMessage(CSteamID, int, void *, int, int *) { return 0; }
	virtual SteamAPICall_t GetFollowerCount(CSteamID) { return 0; }
	virtual SteamAPICall_t IsFollowing(CSteamID) { return 0; }
	virtual SteamAPICall_t EnumerateFollowingList(uint32) { return 0; }
	virtual bool IsClanPublic(CSteamID) { return false; }
	virtual bool IsClanOfficialGameGroup(CSteamID) { return false; }
	virtual int GetNumChatsWithUnreadPriorityMessages() { return 0; }
	virtual void ActivateGameOverlayRemotePlayTogetherInviteDialog(CSteamID) {}
	virtual bool RegisterProtocolInOverlayBrowser(const char *) { return false; }
	virtual void ActivateGameOverlayInviteDialogConnectString(const char *) {}
};

class SteamUtils {
public:
	virtual uint32 GetSecondsSinceAppActive() { return 1; }
	virtual uint32 GetSecondsSinceComputerActive() { return 1; }
	virtual int GetConnectedUniverse() { return k_EUniversePublic; }
	virtual uint32 GetServerRealTime() { return (uint32)time(NULL); }
	virtual const char *GetIPCountry() { return "US"; }
	virtual bool GetImageSize(int, uint32 *, uint32 *) { return false; }
	virtual bool GetImageRGBA(int, uint8 *, int) { return false; }
	virtual bool GetCSERIPPort(uint32 *, uint16 *) { return false; }
	virtual uint8 GetCurrentBatteryPower() { return 255; }
	virtual uint32 GetAppID() { return Vellum_GetIdentity().app_id; }
	virtual void SetOverlayNotificationPosition(int) {}
	virtual bool IsAPICallCompleted(SteamAPICall_t, bool *pbFailed)
	{
		if (pbFailed) *pbFailed = true;
		return true;
	}
	virtual int GetAPICallFailureReason(SteamAPICall_t) { return 1; }
	virtual bool GetAPICallResult(SteamAPICall_t, void *, int, int, bool *pbFailed)
	{
		if (pbFailed) *pbFailed = true;
		return false;
	}
	virtual void RunFrame() { Vellum_FlushListCallbacks(); }
	virtual uint32 GetIPCCallCount() { return 0; }
	virtual void SetWarningMessageHook(SteamAPIWarningMessageHook_t) {}
	virtual bool IsOverlayEnabled() { return false; }
	virtual bool BOverlayNeedsPresent() { return false; }
	virtual SteamAPICall_t CheckFileSignature(const char *) { return 0; }
	virtual bool ShowGamepadTextInput(int, int, const char *, uint32, const char *) { return false; }
	virtual uint32 GetEnteredGamepadTextLength() { return 0; }
	virtual bool GetEnteredGamepadTextInput(char *, uint32) { return false; }
	virtual const char *GetSteamUILanguage() { return "english"; }
	virtual bool IsSteamRunningInVR() { return false; }
	virtual void SetOverlayNotificationInset(int32, int32) {}
	virtual bool IsSteamInBigPictureMode() { return false; }
	virtual void StartVRDashboard() {}
	virtual bool IsVRHeadsetStreamingEnabled() { return false; }
	virtual void SetVRHeadsetStreamingEnabled(bool) {}
	virtual bool IsSteamChinaLauncher() { return false; }
	virtual bool InitFilterText(uint32) { return false; }
	virtual int FilterText(int, CSteamID, const char *in, char *out, uint32 outBytes)
	{
		if (out == NULL || outBytes == 0) {
			return 0;
		}
		if (in == NULL) {
			out[0] = '\0';
			return 0;
		}
		strncpy(out, in, (size_t)outBytes - 1);
		out[outBytes - 1] = '\0';
		return 0;
	}
	virtual int GetIPv6ConnectivityState(int) { return 0; }
	virtual bool IsSteamRunningOnSteamDeck() { return false; }
	virtual bool ShowFloatingGamepadTextInput(int, int, int, int, int) { return false; }
	virtual void SetGameLauncherMode(bool) {}
	virtual bool DismissFloatingGamepadTextInput() { return false; }
	virtual bool DismissGamepadTextInput() { return false; }
};

class SteamApps {
public:
	virtual bool BIsSubscribed() { return true; }
	virtual bool BIsLowViolence() { return false; }
	virtual bool BIsCybercafe() { return false; }
	virtual bool BIsVACBanned() { return false; }
	virtual const char *GetCurrentGameLanguage() { return "english"; }
	virtual const char *GetAvailableGameLanguages() { return "english"; }
	virtual bool BIsSubscribedApp(AppId_t) { return true; }
	virtual bool BIsDlcInstalled(AppId_t) { return true; }
	virtual uint32 GetEarliestPurchaseUnixTime(AppId_t) { return 1; }
	virtual bool BIsSubscribedFromFreeWeekend() { return false; }
	virtual int GetDLCCount() { return 0; }
	virtual bool BGetDLCDataByIndex(int, AppId_t *, bool *, char *, int) { return false; }
	virtual void InstallDLC(AppId_t) {}
	virtual void UninstallDLC(AppId_t) {}
	virtual void RequestAppProofOfPurchaseKey(AppId_t) {}
	virtual bool GetCurrentBetaName(char *pchName, int cchNameBufferSize)
	{
		if (pchName == NULL || cchNameBufferSize <= 0) {
			return false;
		}
		strncpy(pchName, "public", (size_t)cchNameBufferSize - 1);
		pchName[cchNameBufferSize - 1] = '\0';
		return true;
	}
	virtual bool MarkContentCorrupt(bool) { return false; }
	virtual uint32 GetInstalledDepots(AppId_t, DepotId_t *, uint32) { return 0; }
	virtual uint32 GetAppInstallDir(AppId_t, char *pchFolder, uint32 cchFolderBufferSize)
	{
		if (pchFolder == NULL || cchFolderBufferSize == 0) {
			return 0;
		}
		strncpy(pchFolder, ".", cchFolderBufferSize - 1);
		pchFolder[cchFolderBufferSize - 1] = '\0';
		return (uint32)strlen(pchFolder);
	}
	virtual bool BIsAppInstalled(AppId_t) { return true; }
	virtual CSteamID GetAppOwner() { return Vellum_GetIdentity().steam_id; }
	virtual const char *GetLaunchQueryParam(const char *) { return ""; }
	virtual bool GetDlcDownloadProgress(AppId_t, uint64 *dl, uint64 *total)
	{
		if (dl) *dl = 0;
		if (total) *total = 0;
		return false;
	}
	virtual int GetAppBuildId() { return 0; }
	virtual void RequestAllProofOfPurchaseKeys() {}
	virtual SteamAPICall_t GetFileDetails(const char *) { return 0; }
	virtual int GetLaunchCommandLine(char *psz, int cub)
	{
		if (psz != NULL && cub > 0) {
			psz[0] = '\0';
		}
		return 0;
	}
	virtual bool BIsSubscribedFromFamilySharing() { return false; }
	virtual bool BIsTimedTrial(uint32 *allowed, uint32 *played)
	{
		if (allowed) *allowed = 0;
		if (played) *played = 0;
		return false;
	}
	virtual bool SetDlcContext(AppId_t) { return true; }
};

enum { k_unFavoriteFlagFavorite = 0x01 };
enum { k_unFavoriteFlagHistory = 0x02 };

static int Vellum_FavCount();
static bool Vellum_FavGet(int i, AppId_t *app, uint32 *ip, uint16 *conn, uint16 *query, uint32 *flags, uint32 *played);
static int Vellum_FavAdd(AppId_t app, uint32 ip, uint16 conn, uint16 query, uint32 flags, uint32 played);
static bool Vellum_FavRemove(AppId_t app, uint32 ip, uint16 conn, uint16 query, uint32 flags);
static HServerListRequest Vellum_ListRequest(AppId_t app, uint32 flagMask, void *response);
static void Vellum_ListRelease(HServerListRequest h);
static void *Vellum_ListDetails(HServerListRequest h, int i);
static int Vellum_ListCount(HServerListRequest h);

class SteamHTTP {
public:
	virtual HTTPRequestHandle CreateHTTPRequest(int, const char *) { return 0; }
	virtual bool SetHTTPRequestContextValue(HTTPRequestHandle, uint64) { return false; }
	virtual bool SetHTTPRequestNetworkActivityTimeout(HTTPRequestHandle, uint32) { return false; }
	virtual bool SetHTTPRequestHeaderValue(HTTPRequestHandle, const char *, const char *) { return false; }
	virtual bool SetHTTPRequestGetOrPostParameter(HTTPRequestHandle, const char *, const char *) { return false; }
	virtual bool SendHTTPRequest(HTTPRequestHandle, SteamAPICall_t *) { return false; }
	virtual bool SendHTTPRequestAndStreamResponse(HTTPRequestHandle, SteamAPICall_t *) { return false; }
	virtual bool DeferHTTPRequest(HTTPRequestHandle) { return false; }
	virtual bool PrioritizeHTTPRequest(HTTPRequestHandle) { return false; }
	virtual bool GetHTTPResponseHeaderSize(HTTPRequestHandle, const char *, uint32 *) { return false; }
	virtual bool GetHTTPResponseHeaderValue(HTTPRequestHandle, const char *, uint8 *, uint32) { return false; }
	virtual bool GetHTTPResponseBodySize(HTTPRequestHandle, uint32 *) { return false; }
	virtual bool GetHTTPResponseBodyData(HTTPRequestHandle, uint8 *, uint32) { return false; }
	virtual bool GetHTTPStreamingResponseBodyData(HTTPRequestHandle, uint32, uint8 *, uint32) { return false; }
	virtual bool ReleaseHTTPRequest(HTTPRequestHandle) { return false; }
	virtual bool GetHTTPDownloadProgressPct(HTTPRequestHandle, float *) { return false; }
	virtual bool SetHTTPRequestRawPostBody(HTTPRequestHandle, const char *, uint8 *, uint32) { return false; }
	virtual bool SetHTTPRequestAbsoluteTimeoutMS(HTTPRequestHandle, uint32) { return false; }
	virtual bool GetHTTPRequestWasTimedOut(HTTPRequestHandle, bool *out)
	{
		if (out) *out = false;
		return false;
	}
};

class SteamMatchmaking {
public:
	virtual int GetFavoriteGameCount() { return Vellum_FavCount(); }
	virtual bool GetFavoriteGame(int i, AppId_t *app, uint32 *ip, uint16 *conn, uint16 *query, uint32 *flags, uint32 *played)
	{
		return Vellum_FavGet(i, app, ip, conn, query, flags, played);
	}
	virtual int AddFavoriteGame(AppId_t app, uint32 ip, uint16 conn, uint16 query, uint32 flags, uint32 played)
	{
		Vellum_Log("AddFavoriteGame ip=%u conn=%u flags=%u", ip, (unsigned)conn, flags);
		return Vellum_FavAdd(app, ip, conn, query, flags, played);
	}
	virtual bool RemoveFavoriteGame(AppId_t app, uint32 ip, uint16 conn, uint16 query, uint32 flags)
	{
		return Vellum_FavRemove(app, ip, conn, query, flags);
	}
	virtual SteamAPICall_t RequestLobbyList() { return 0; }
	virtual void AddRequestLobbyListStringFilter(const char *, const char *, int) {}
	virtual void AddRequestLobbyListNumericalFilter(const char *, int, int) {}
	virtual void AddRequestLobbyListNearValueFilter(const char *, int) {}
	virtual void AddRequestLobbyListFilterSlotsAvailable(int) {}
	virtual void AddRequestLobbyListDistanceFilter(int) {}
	virtual void AddRequestLobbyListResultCountFilter(int) {}
	virtual void AddRequestLobbyListCompatibleMembersFilter(CSteamID) {}
	virtual CSteamID GetLobbyByIndex(int) { return CSteamID(); }
	virtual SteamAPICall_t CreateLobby(int, int) { return 0; }
	virtual SteamAPICall_t JoinLobby(CSteamID) { return 0; }
	virtual void LeaveLobby(CSteamID) {}
	virtual bool InviteUserToLobby(CSteamID, CSteamID) { return false; }
	virtual int GetNumLobbyMembers(CSteamID) { return 0; }
	virtual CSteamID GetLobbyMemberByIndex(CSteamID, int) { return CSteamID(); }
	virtual const char *GetLobbyData(CSteamID, const char *) { return ""; }
	virtual bool SetLobbyData(CSteamID, const char *, const char *) { return false; }
	virtual int GetLobbyDataCount(CSteamID) { return 0; }
	virtual bool GetLobbyDataByIndex(CSteamID, int, char *, int, char *, int) { return false; }
	virtual bool DeleteLobbyData(CSteamID, const char *) { return false; }
	virtual const char *GetLobbyMemberData(CSteamID, CSteamID, const char *) { return ""; }
	virtual void SetLobbyMemberData(CSteamID, const char *, const char *) {}
	virtual bool SendLobbyChatMsg(CSteamID, const void *, int) { return false; }
	virtual int GetLobbyChatEntry(CSteamID, int, CSteamID *, void *, int, int *) { return 0; }
	virtual bool RequestLobbyData(CSteamID) { return false; }
	virtual void SetLobbyGameServer(CSteamID, uint32, uint16, CSteamID) {}
	virtual bool GetLobbyGameServer(CSteamID, uint32 *, uint16 *, CSteamID *) { return false; }
	virtual bool SetLobbyMemberLimit(CSteamID, int) { return false; }
	virtual int GetLobbyMemberLimit(CSteamID) { return 0; }
	virtual bool SetLobbyType(CSteamID, int) { return false; }
	virtual bool SetLobbyJoinable(CSteamID, bool) { return false; }
	virtual CSteamID GetLobbyOwner(CSteamID) { return CSteamID(); }
	virtual bool SetLobbyOwner(CSteamID, CSteamID) { return false; }
	virtual bool SetLinkedLobby(CSteamID, CSteamID) { return false; }
};

class SteamMatchmakingServers {
public:
	virtual HServerListRequest RequestInternetServerList(AppId_t, void **, uint32, void *) { return NULL; }
	virtual HServerListRequest RequestLANServerList(AppId_t, void *) { return NULL; }
	virtual HServerListRequest RequestFriendsServerList(AppId_t, void **, uint32, void *) { return NULL; }
	virtual HServerListRequest RequestFavoritesServerList(AppId_t app, void **, uint32, void *cb)
	{
		return Vellum_ListRequest(app, k_unFavoriteFlagFavorite, cb);
	}
	virtual HServerListRequest RequestHistoryServerList(AppId_t app, void **, uint32, void *cb)
	{
		return Vellum_ListRequest(app, k_unFavoriteFlagHistory, cb);
	}
	virtual HServerListRequest RequestSpectatorServerList(AppId_t, void **, uint32, void *) { return NULL; }
	virtual void ReleaseRequest(HServerListRequest h) { Vellum_ListRelease(h); }
	virtual void *GetServerDetails(HServerListRequest h, int i) { return Vellum_ListDetails(h, i); }
	virtual void CancelQuery(HServerListRequest) {}
	virtual void RefreshQuery(HServerListRequest) {}
	virtual bool IsRefreshing(HServerListRequest) { return false; }
	virtual int GetServerCount(HServerListRequest h) { return Vellum_ListCount(h); }
	virtual void RefreshServer(HServerListRequest, int) {}
	virtual HServerQuery PingServer(uint32, uint16, void *) { return 0; }
	virtual HServerQuery PlayerDetails(uint32, uint16, void *) { return 0; }
	virtual HServerQuery ServerRules(uint32, uint16, void *) { return 0; }
	virtual void CancelServerQuery(HServerQuery) {}
};

class SteamUserStats {
public:
	virtual bool RequestCurrentStats() { return true; }
	virtual bool GetStat(const char *, int32 *) { return false; }
	virtual bool GetStat(const char *, float *) { return false; }
	virtual bool SetStat(const char *, int32) { return false; }
	virtual bool SetStat(const char *, float) { return false; }
	virtual bool UpdateAvgRateStat(const char *, float, double) { return false; }
	virtual bool GetAchievement(const char *, bool *pbAchieved)
	{
		if (pbAchieved) *pbAchieved = false;
		return false;
	}
	virtual bool SetAchievement(const char *) { return false; }
	virtual bool ClearAchievement(const char *) { return false; }
	virtual bool GetAchievementAndUnlockTime(const char *, bool *, uint32 *) { return false; }
	virtual bool StoreStats() { return true; }
	virtual int GetAchievementIcon(const char *) { return 0; }
	virtual const char *GetAchievementDisplayAttribute(const char *, const char *) { return ""; }
	virtual bool IndicateAchievementProgress(const char *, uint32, uint32) { return false; }
	virtual uint32 GetNumAchievements() { return 0; }
	virtual const char *GetAchievementName(uint32) { return ""; }
	virtual SteamAPICall_t RequestUserStats(CSteamID) { return 0; }
	virtual bool GetUserStat(CSteamID, const char *, int32 *) { return false; }
	virtual bool GetUserStat(CSteamID, const char *, float *) { return false; }
	virtual bool GetUserAchievement(CSteamID, const char *, bool *) { return false; }
	virtual bool GetUserAchievementAndUnlockTime(CSteamID, const char *, bool *, uint32 *) { return false; }
	virtual bool ResetAllStats(bool) { return false; }
	virtual SteamAPICall_t FindOrCreateLeaderboard(const char *, int, int) { return 0; }
	virtual SteamAPICall_t FindLeaderboard(const char *) { return 0; }
	virtual const char *GetLeaderboardName(SteamLeaderboard_t) { return ""; }
	virtual int GetLeaderboardEntryCount(SteamLeaderboard_t) { return 0; }
	virtual int GetLeaderboardSortMethod(SteamLeaderboard_t) { return 0; }
	virtual int GetLeaderboardDisplayType(SteamLeaderboard_t) { return 0; }
	virtual SteamAPICall_t DownloadLeaderboardEntries(SteamLeaderboard_t, int, int, int) { return 0; }
	virtual SteamAPICall_t DownloadLeaderboardEntriesForUsers(SteamLeaderboard_t, CSteamID *, int) { return 0; }
	virtual bool GetDownloadedLeaderboardEntry(SteamLeaderboardEntries_t, int, LeaderboardEntry_t *, int32[], int) { return false; }
	virtual SteamAPICall_t UploadLeaderboardScore(SteamLeaderboard_t, int, int32, const int32 *, int) { return 0; }
	virtual SteamAPICall_t AttachLeaderboardUGC(SteamLeaderboard_t, UGCHandle_t) { return 0; }
	virtual SteamAPICall_t GetNumberOfCurrentPlayers() { return 0; }
	virtual SteamAPICall_t RequestGlobalAchievementPercentages() { return 0; }
	virtual int GetMostAchievedAchievementInfo(char *, uint32, float *, bool *) { return -1; }
	virtual int GetNextMostAchievedAchievementInfo(int, char *, uint32, float *, bool *) { return -1; }
	virtual bool GetAchievementAchievedPercent(const char *, float *) { return false; }
	virtual SteamAPICall_t RequestGlobalStats(int) { return 0; }
	virtual bool GetGlobalStat(const char *, int64 *) { return false; }
	virtual bool GetGlobalStat(const char *, double *) { return false; }
	virtual int32 GetGlobalStatHistory(const char *, int64 *, uint32) { return 0; }
	virtual int32 GetGlobalStatHistory(const char *, double *, uint32) { return 0; }
};

class SteamNetworking {
public:
	virtual bool SendP2PPacket(CSteamID, const void *, uint32, int, int) { return false; }
	virtual bool IsP2PPacketAvailable(uint32 *, int) { return false; }
	virtual bool ReadP2PPacket(void *, uint32, uint32 *, CSteamID *, int) { return false; }
	virtual bool AcceptP2PSessionWithUser(CSteamID) { return false; }
	virtual bool CloseP2PSessionWithUser(CSteamID) { return false; }
	virtual bool CloseP2PChannelWithUser(CSteamID, int) { return false; }
	virtual bool GetP2PSessionState(CSteamID, P2PSessionState_t *) { return false; }
	virtual bool AllowP2PPacketRelay(bool) { return true; }
	virtual SNetListenSocket_t CreateListenSocket(int, uint32, uint16, bool) { return 0; }
	virtual SNetSocket_t CreateP2PConnectionSocket(CSteamID, int, int, bool) { return 0; }
	virtual SNetSocket_t CreateConnectionSocket(uint32, uint16, int) { return 0; }
	virtual bool DestroySocket(SNetSocket_t, bool) { return false; }
	virtual bool DestroyListenSocket(SNetListenSocket_t, bool) { return false; }
	virtual bool SendDataOnSocket(SNetSocket_t, void *, uint32, bool) { return false; }
	virtual bool IsDataAvailableOnSocket(SNetSocket_t, uint32 *) { return false; }
	virtual bool RetrieveDataFromSocket(SNetSocket_t, void *, uint32, uint32 *) { return false; }
	virtual bool IsDataAvailable(SNetListenSocket_t, uint32 *, SNetSocket_t *) { return false; }
	virtual bool RetrieveData(SNetListenSocket_t, void *, uint32, uint32 *, SNetSocket_t *) { return false; }
	virtual bool GetSocketInfo(SNetSocket_t, CSteamID *, int *, uint32 *, uint16 *) { return false; }
	virtual bool GetListenSocketInfo(SNetListenSocket_t, uint32 *, uint16 *) { return false; }
	virtual int GetSocketConnectionType(SNetSocket_t) { return 0; }
	virtual int GetMaxPacketSize(SNetSocket_t) { return 1200; }
};

static void Vellum_FillNetIdentity(void *pIdentity)
{
	uint8 *p = (uint8 *)pIdentity;
	if (p == NULL) {
		return;
	}
	memset(p, 0, 136);
	*(int32 *)p = 16; /* k_ESteamNetworkingIdentityType_SteamID */
	*(int32 *)(p + 4) = 8;
	*(uint64 *)(p + 8) = Vellum_GetIdentity().steam_id.ConvertToUint64();
}

/* ISteamNetworkingSockets012. Must not share the ISteamNetworking vtable:
 * steam_api asks for SteamNetworkingSockets012, which used to match
 * strncmp("SteamNetworking") and return the 22-slot P2P iface. */
class SteamNetworkingSockets {
public:
	virtual uint32 CreateListenSocketIP(const void *, int, const void *) { return 0; }
	virtual uint32 ConnectByIPAddress(const void *, int, const void *)
	{
		Vellum_Log("ConnectByIPAddress");
		return 0;
	}
	virtual uint32 CreateListenSocketP2P(int, int, const void *) { return 0; }
	virtual uint32 ConnectP2P(const void *, int, int, const void *) { return 0; }
	virtual int AcceptConnection(uint32) { return 2; }
	virtual bool CloseConnection(uint32, int, const char *, bool) { return true; }
	virtual bool CloseListenSocket(uint32) { return true; }
	virtual bool SetConnectionUserData(uint32, int64) { return false; }
	virtual int64 GetConnectionUserData(uint32) { return 0; }
	virtual void SetConnectionName(uint32, const char *) {}
	virtual bool GetConnectionName(uint32, char *name, int maxLen)
	{
		if (name != NULL && maxLen > 0) {
			name[0] = '\0';
		}
		return false;
	}
	virtual int SendMessageToConnection(uint32, const void *, uint32, int, int64 *) { return 3; }
	virtual void SendMessages(int, void *const *, int64 *) {}
	virtual int FlushMessagesOnConnection(uint32) { return 3; }
	virtual int ReceiveMessagesOnConnection(uint32, void **, int) { return 0; }
	virtual bool GetConnectionInfo(uint32, void *) { return false; }
	virtual int GetConnectionRealTimeStatus(uint32, void *, int, void *) { return 3; }
	virtual int GetDetailedConnectionStatus(uint32, char *buf, int cb)
	{
		if (buf != NULL && cb > 0) {
			buf[0] = '\0';
		}
		return 0;
	}
	virtual bool GetListenSocketAddress(uint32, void *) { return false; }
	virtual bool CreateSocketPair(uint32 *, uint32 *, bool, const void *, const void *) { return false; }
	virtual bool ConfigureConnectionLanes(uint32, int, const int *, const uint16 *) { return false; }
	virtual bool GetIdentity(void *id)
	{
		Vellum_Log("GetIdentity");
		Vellum_FillNetIdentity(id);
		return true;
	}
	virtual int InitAuthentication()
	{
		Vellum_Log("InitAuthentication");
		return 100;
	}
	virtual int GetAuthenticationStatus(void *) { return 100; }
	virtual uint32 CreatePollGroup() { return 0; }
	virtual bool DestroyPollGroup(uint32) { return false; }
	virtual bool SetConnectionPollGroup(uint32, uint32) { return false; }
	virtual int ReceiveMessagesOnPollGroup(uint32, void **, int) { return 0; }
	virtual bool ReceivedRelayAuthTicket(const void *, int, void *) { return false; }
	virtual int FindRelayAuthTicketForServer(const void *, int, void *) { return 0; }
	virtual uint32 ConnectToHostedDedicatedServer(const void *, int, int, const void *) { return 0; }
	virtual uint16 GetHostedDedicatedServerPort() { return 0; }
	virtual uint32 GetHostedDedicatedServerPOPID() { return 0; }
	virtual int GetHostedDedicatedServerAddress(void *) { return 2; }
	virtual uint32 CreateHostedDedicatedServerListenSocket(int, int, const void *) { return 0; }
	virtual int GetGameCoordinatorServerLogin(void *, int *, void *) { return 2; }
	virtual uint32 ConnectP2PCustomSignaling(void *, const void *, int, int, const void *) { return 0; }
	virtual bool ReceivedP2PCustomSignal(const void *, int, void *) { return false; }
	virtual bool GetCertificateRequest(int *, void *, void *) { return false; }
	virtual bool SetCertificate(const void *, int, void *) { return false; }
	virtual void ResetIdentity(const void *) {}
	virtual void RunCallbacks() { Vellum_FlushListCallbacks(); }
	virtual bool BeginAsyncRequestFakeIP(int) { return false; }
	virtual void GetFakeIP(int, void *) {}
	virtual uint32 CreateListenSocketP2PFakeIP(int, int, const void *) { return 0; }
	virtual int GetRemoteFakeIPForConnection(uint32, void *) { return 2; }
	virtual void *CreateFakeUDPPort(int) { return NULL; }
};

class SteamNetworkingUtils {
public:
	virtual void *AllocateMessage(int) { return NULL; }
	virtual int GetRelayNetworkStatus(void *details)
	{
		if (details != NULL) {
			int *d = (int *)details;
			d[0] = 100;
			d[1] = 0;
			d[2] = 100;
			d[3] = 100;
		}
		return 100;
	}
	virtual float GetLocalPingLocation(void *) { return -1.0f; }
	virtual int EstimatePingTimeBetweenTwoLocations(const void *, const void *) { return -1; }
	virtual int EstimatePingTimeFromLocalHost(const void *) { return -1; }
	virtual void ConvertPingLocationToString(const void *, char *buf, int n)
	{
		if (buf != NULL && n > 0) {
			buf[0] = '\0';
		}
	}
	virtual bool ParsePingLocationString(const char *, void *) { return false; }
	virtual bool CheckPingDataUpToDate(float) { return true; }
	virtual int GetPingToDataCenter(uint32, uint32 *) { return -1; }
	virtual int GetDirectPingToPOP(uint32) { return -1; }
	virtual int GetPOPCount() { return 0; }
	virtual int GetPOPList(uint32 *, int) { return 0; }
	virtual int64 GetLocalTimestamp()
	{
#ifdef _WIN32
		return (int64)GetTickCount() * 1000;
#else
		return 1;
#endif
	}
	virtual void SetDebugOutputFunction(int, void *) {}
	virtual int GetIPv4FakeIPType(uint32) { return 0; }
	virtual int GetRealIdentityForFakeIP(const void *, void *) { return 2; }
	virtual bool SetConfigValue(int, int, size_t, int, const void *) { return true; }
	virtual int GetConfigValue(int, int, size_t, int *, void *, size_t *) { return 0; }
	virtual const char *GetConfigValueInfo(int, int *, int *) { return ""; }
	virtual int IterateGenericEditableConfigValues(int, bool) { return 0; }
	virtual void SteamNetworkingIPAddr_ToString(const void *, char *buf, size_t n, bool)
	{
		if (buf != NULL && n > 0) {
			buf[0] = '\0';
		}
	}
	virtual bool SteamNetworkingIPAddr_ParseString(void *, const char *) { return false; }
	virtual int SteamNetworkingIPAddr_GetFakeIPType(const void *) { return 0; }
	virtual void SteamNetworkingIdentity_ToString(const void *, char *buf, size_t n)
	{
		if (buf != NULL && n > 0) {
			buf[0] = '\0';
		}
	}
	virtual bool SteamNetworkingIdentity_ParseString(void *, const char *) { return false; }
};

class SteamRemoteStorage {
public:
	virtual bool FileWrite(const char *, const void *, int32) { return false; }
	virtual int32 FileRead(const char *, void *, int32) { return 0; }
	virtual bool FileForget(const char *) { return false; }
	virtual bool FileDelete(const char *) { return false; }
	virtual SteamAPICall_t FileShare(const char *) { return 0; }
	virtual bool SetSyncPlatforms(const char *, int) { return false; }
	virtual GID_t FileWriteStreamOpen(const char *) { return 0; }
	virtual int FileWriteStreamWriteChunk(GID_t, const void *, int32) { return 2; } /* k_EResultFail */
	virtual int FileWriteStreamClose(GID_t) { return 2; }
	virtual int FileWriteStreamCancel(GID_t) { return 2; }
	virtual bool FileExists(const char *) { return false; }
	virtual bool FilePersisted(const char *) { return false; }
	virtual int32 GetFileSize(const char *) { return 0; }
	virtual int64 GetFileTimestamp(const char *) { return 0; }
	virtual int GetSyncPlatforms(const char *) { return 0; }
	virtual int32 GetFileCount() { return 0; }
	virtual const char *GetFileNameAndSize(int, int32 *pnFileSizeInBytes)
	{
		if (pnFileSizeInBytes) *pnFileSizeInBytes = 0;
		return "";
	}
	virtual bool GetQuota(int32 *pnTotalBytes, int32 *puAvailableBytes)
	{
		if (pnTotalBytes) *pnTotalBytes = 0;
		if (puAvailableBytes) *puAvailableBytes = 0;
		return true;
	}
	virtual bool IsCloudEnabledForAccount() { return false; }
	virtual bool IsCloudEnabledForApp() { return false; }
	virtual void SetCloudEnabledForApp(bool) {}
	virtual SteamAPICall_t UGCDownload(UGCHandle_t, uint32) { return 0; }
	virtual bool GetUGCDownloadProgress(UGCHandle_t, uint32 *, uint32 *) { return false; }
	virtual bool GetUGCDetails(UGCHandle_t, AppId_t *, char **, int32 *, CSteamID *) { return false; }
	virtual int32 UGCRead(UGCHandle_t, void *, int32, uint32, int) { return 0; }
	virtual int32 GetCachedUGCCount() { return 0; }
	virtual UGCHandle_t GetCachedUGCHandle(int32) { return 0; }
	virtual SteamAPICall_t PublishWorkshopFile(const char *, const char *, AppId_t, const char *, const char *, int, SteamParamStringArray_t *, int) { return 0; }
	virtual JobID_t CreatePublishedFileUpdateRequest(PublishedFileId_t) { return 0; }
	virtual bool UpdatePublishedFileFile(JobID_t, const char *) { return false; }
	virtual bool UpdatePublishedFilePreviewFile(JobID_t, const char *) { return false; }
	virtual bool UpdatePublishedFileTitle(JobID_t, const char *) { return false; }
	virtual bool UpdatePublishedFileDescription(JobID_t, const char *) { return false; }
	virtual bool UpdatePublishedFileVisibility(JobID_t, int) { return false; }
	virtual bool UpdatePublishedFileTags(JobID_t, SteamParamStringArray_t *) { return false; }
	virtual SteamAPICall_t CommitPublishedFileUpdate(JobID_t) { return 0; }
	virtual SteamAPICall_t GetPublishedFileDetails(PublishedFileId_t, uint32) { return 0; }
	virtual SteamAPICall_t DeletePublishedFile(PublishedFileId_t) { return 0; }
	virtual SteamAPICall_t EnumerateUserPublishedFiles(uint32) { return 0; }
	virtual SteamAPICall_t SubscribePublishedFile(PublishedFileId_t) { return 0; }
	virtual SteamAPICall_t EnumerateUserSubscribedFiles(uint32) { return 0; }
	virtual SteamAPICall_t UnsubscribePublishedFile(PublishedFileId_t) { return 0; }
	virtual bool UpdatePublishedFileSetChangeDescription(JobID_t, const char *) { return false; }
	virtual SteamAPICall_t GetPublishedItemVoteDetails(PublishedFileId_t) { return 0; }
	virtual SteamAPICall_t UpdateUserPublishedItemVote(PublishedFileId_t, bool) { return 0; }
	virtual SteamAPICall_t GetUserPublishedItemVoteDetails(PublishedFileId_t) { return 0; }
	virtual SteamAPICall_t EnumerateUserSharedWorkshopFiles(AppId_t, CSteamID, uint32, SteamParamStringArray_t *, SteamParamStringArray_t *) { return 0; }
	virtual SteamAPICall_t PublishVideo(int, const char *, const char *, const char *, AppId_t, const char *, const char *, int, SteamParamStringArray_t *) { return 0; }
	virtual SteamAPICall_t SetUserPublishedFileAction(PublishedFileId_t, int) { return 0; }
	virtual SteamAPICall_t EnumeratePublishedFilesByUserAction(int, uint32) { return 0; }
	virtual SteamAPICall_t EnumeratePublishedWorkshopFiles(int, uint32, uint32, uint32, SteamParamStringArray_t *, SteamParamStringArray_t *) { return 0; }
	virtual SteamAPICall_t UGCDownloadToLocation(UGCHandle_t, const char *, uint32) { return 0; }
};

class SteamScreenshots {
public:
	virtual ScreenshotHandle WriteScreenshot(void *, uint32, int, int) { return 0; }
	virtual ScreenshotHandle AddScreenshotToLibrary(const char *, const char *, int, int) { return 0; }
	virtual void TriggerScreenshot() {}
	virtual void HookScreenshots(bool) {}
	virtual bool SetLocation(ScreenshotHandle, const char *) { return false; }
	virtual bool TagUser(ScreenshotHandle, CSteamID) { return false; }
	virtual bool TagPublishedFile(ScreenshotHandle, PublishedFileId_t) { return false; }
};

class SteamUnifiedMessages {
public:
	virtual uint64 SendMethod(const char *, const void *, uint32, uint64) { return 0; }
	virtual bool GetMethodResponseInfo(uint64, uint32 *, int *) { return false; }
	virtual bool GetMethodResponseData(uint64, void *, uint32, bool) { return false; }
	virtual bool ReleaseMethod(uint64) { return false; }
	virtual bool SendNotification(const char *, const void *, uint32) { return false; }
};

static SteamUser g_user;
static SteamUser023 g_user023;
static SteamFriends g_friends;
static SteamFriends017 g_friends017;
static SteamUtils g_utils;
static SteamApps g_apps;
static SteamHTTP g_http;
static SteamMatchmaking g_mm;
static SteamMatchmakingServers g_mms;
static SteamUserStats g_stats;
static SteamNetworking g_net;
static SteamNetworkingSockets g_netsockets;
static SteamNetworkingUtils g_netutils;
static SteamRemoteStorage g_remote;
static SteamScreenshots g_shots;
static SteamUnifiedMessages g_unified;

static void *Vellum_PickUser(const char *ver)
{
	Vellum_Log("GetISteamUser %s", ver ? ver : "(null)");
	if (ver != NULL && (strstr(ver, "021") || strstr(ver, "022") || strstr(ver, "023") || strstr(ver, "024"))) {
		return &g_user023;
	}
	return &g_user;
}

static void *Vellum_PickFriends(const char *ver)
{
	Vellum_Log("GetISteamFriends %s", ver ? ver : "(null)");
	if (ver != NULL && (strstr(ver, "016") || strstr(ver, "017") || strstr(ver, "018"))) {
		return &g_friends017;
	}
	return &g_friends;
}

enum { k_iSteamUserCallbacks = 100 };
enum { k_iSteamGameServerCallbacks = 200 };
enum { k_iCallback_SteamServersConnected = k_iSteamUserCallbacks + 1 };
enum { k_iCallback_GSPolicyResponse = k_iSteamUserCallbacks + 15 };
enum { k_iCallback_GSClientApprove = k_iSteamGameServerCallbacks + 1 };
enum { k_iCallback_ValidateAuthTicket = k_iSteamUserCallbacks + 43 };
enum { k_iCallback_GetAuthSessionTicketResponse = k_iSteamUserCallbacks + 63 };

struct GSPolicyResponse_t {
	uint8 m_bSecure;
};

struct GSClientApprove_t {
	CSteamID m_SteamID;
	CSteamID m_OwnerSteamID;
};

struct ValidateAuthTicketResponse_t {
	CSteamID m_SteamID;
	int m_eAuthSessionResponse;
	CSteamID m_OwnerSteamID;
};

struct GetAuthSessionTicketResponse_t {
	HAuthTicket m_hAuthTicket;
	int m_eResult;
};

#define VELLUM_CB_QUEUE 8

struct VellumQueuedCallback {
	HSteamUser user;
	int id;
	int size;
	uint8 data[256];
};

static VellumQueuedCallback g_cbq[VELLUM_CB_QUEUE];
static int g_cbq_head;
static int g_cbq_count;
static VellumQueuedCallback g_cb_last;
static int g_cb_have_last;
static int g_gs_logged_on;
static uint32 g_gs_bots;

static void Vellum_QueueCallback(HSteamUser user, int id, const void *data, int size)
{
	VellumQueuedCallback *slot;
	if (size < 0 || size > (int)sizeof(g_cbq[0].data)) {
		return;
	}
	if (g_cbq_count >= VELLUM_CB_QUEUE) {
		return;
	}
	slot = &g_cbq[(g_cbq_head + g_cbq_count) % VELLUM_CB_QUEUE];
	slot->user = user;
	slot->id = id;
	slot->size = size;
	if (size > 0 && data != NULL) {
		memcpy(slot->data, data, (size_t)size);
	}
	g_cbq_count++;
}

static int Vellum_FillAuthTicket(void *pTicket, int cbMaxTicket, uint32 *pcbTicket)
{
	GetAuthSessionTicketResponse_t r;
	int n = Vellum_WriteAuthBlob(pTicket, cbMaxTicket);
	if (pcbTicket) {
		*pcbTicket = (uint32)n;
	}
	Vellum_Log("FillAuthTicket wrote=%d", n);
	if (n <= 0) {
		return 0;
	}
	r.m_hAuthTicket = 1;
	r.m_eResult = 1;
	Vellum_QueueCallback(1, k_iCallback_GetAuthSessionTicketResponse, &r, (int)sizeof(r));
	return n;
}

#define VELLUM_FAV_MAX 64
#define VELLUM_REQ_MAX 4

struct VellumFav {
	AppId_t app;
	uint32 ip;
	uint16 conn;
	uint16 query;
	uint32 flags;
	uint32 played;
};

struct VellumServerNetAdr {
	uint16 conn;
	uint16 query;
	uint32 ip;
};

struct VellumGameServerItem {
	VellumServerNetAdr adr;
	int ping;
	bool hadResponse;
	bool doNotRefresh;
	char gameDir[32];
	char map[32];
	char desc[64];
	uint32 appId;
	int players;
	int maxPlayers;
	int bots;
	bool password;
	bool secure;
	uint32 lastPlayed;
	int version;
	char name[64];
	char tags[128];
	uint64 steamId;
};

struct VellumListReq {
	int used;
	int count;
	int notified;
	void *cb;
	VellumGameServerItem items[VELLUM_FAV_MAX];
};

class SteamServerListResponse {
public:
	virtual void ServerResponded(HServerListRequest, int) = 0;
	virtual void ServerFailedToRespond(HServerListRequest, int) = 0;
	virtual void RefreshComplete(HServerListRequest, int) = 0;
};

static VellumFav g_favs[VELLUM_FAV_MAX];
static int g_fav_n;
static int g_fav_loaded;
static VellumListReq g_reqs[VELLUM_REQ_MAX];

static int Vellum_FavFind(AppId_t app, uint32 ip, uint16 conn, uint16 query);

static void Vellum_GameDir(char *dir, size_t dirSize)
{
#ifdef _WIN32
	HMODULE mod = NULL;
	char *slash;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCSTR)&Vellum_GameDir, &mod);
	GetModuleFileNameA(mod, dir, (DWORD)dirSize);
	dir[dirSize - 1] = '\0';
	slash = strrchr(dir, '\\');
	if (slash != NULL) {
		slash[1] = '\0';
	}
#else
	ssize_t n = readlink("/proc/self/exe", dir, dirSize - 1);
	char *slash;
	if (n > 0) {
		dir[n] = '\0';
		slash = strrchr(dir, '/');
		if (slash != NULL) {
			slash[1] = '\0';
		}
	} else {
		strncpy(dir, "./", dirSize - 1);
		dir[dirSize - 1] = '\0';
	}
#endif
}

static int Vellum_ParseAddr(const char *s, uint32 *ip, uint16 *port)
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

static void Vellum_FmtAddr(char *out, size_t outSize, uint32 ip, uint16 port)
{
	unsigned char *b = (unsigned char *)&ip;
#ifdef _WIN32
	_snprintf(out, outSize, "%u.%u.%u.%u:%u",
	          (unsigned)b[3], (unsigned)b[2], (unsigned)b[1], (unsigned)b[0], (unsigned)port);
#else
	snprintf(out, outSize, "%u.%u.%u.%u:%u",
	         (unsigned)b[3], (unsigned)b[2], (unsigned)b[1], (unsigned)b[0], (unsigned)port);
#endif
	out[outSize - 1] = '\0';
}

static void Vellum_FavAddParsed(const char *addr, uint32 app, uint32 played, uint32 flags)
{
	uint32 ip = 0;
	uint16 port = 0;
	int i;
	if (!Vellum_ParseAddr(addr, &ip, &port)) {
		return;
	}
	i = Vellum_FavFind(app ? (AppId_t)app : 10, ip, port, port);
	if (i >= 0) {
		g_favs[i].flags |= flags;
		if (played) {
			g_favs[i].played = played;
		}
		return;
	}
	if (g_fav_n >= VELLUM_FAV_MAX) {
		return;
	}
	g_favs[g_fav_n].app = app ? (AppId_t)app : 10;
	g_favs[g_fav_n].ip = ip;
	g_favs[g_fav_n].conn = port;
	g_favs[g_fav_n].query = port;
	g_favs[g_fav_n].flags = flags;
	g_favs[g_fav_n].played = played;
	g_fav_n++;
}

static void Vellum_FavLoadFile(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];
	char section[32] = "";
	char addr[64] = "";
	unsigned app = 10;
	unsigned played = 0;
	if (f == NULL) {
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strstr(line, "\"favorites\"") != NULL && strchr(line, '{') == NULL) {
			/* opening key on its own line or with { */
		}
		if (strstr(line, "\"favorites\"") != NULL) {
			strncpy(section, "favorites", sizeof(section) - 1);
			addr[0] = '\0';
			app = 10;
			played = 0;
			continue;
		}
		if (strstr(line, "\"history\"") != NULL) {
			strncpy(section, "history", sizeof(section) - 1);
			addr[0] = '\0';
			app = 10;
			played = 0;
			continue;
		}
		if (strstr(line, "\"Filters\"") != NULL || strstr(line, "\"filters\"") != NULL) {
			if (strstr(line, "\"gamelist\"") == NULL) {
				section[0] = '\0';
			}
		}
		if (strstr(line, "\"address\"") != NULL) {
			char *q1 = strchr(line, '"');
			char *q2 = NULL;
			char *q3 = NULL;
			char *q4 = NULL;
			if (q1) q2 = strchr(q1 + 1, '"');
			if (q2) q3 = strchr(q2 + 1, '"');
			if (q3) q4 = strchr(q3 + 1, '"');
			if (q3 && q4 && (q4 - q3 - 1) < (int)sizeof(addr)) {
				size_t n = (size_t)(q4 - q3 - 1);
				memcpy(addr, q3 + 1, n);
				addr[n] = '\0';
			}
		}
		if (strstr(line, "\"appID\"") != NULL || strstr(line, "\"appid\"") != NULL) {
			char *p = strrchr(line, '"');
			char *p0;
			if (p != NULL) {
				*p = '\0';
				p0 = strrchr(line, '"');
				if (p0 != NULL) {
					app = (unsigned)strtoul(p0 + 1, NULL, 10);
				}
			}
		}
		if (strstr(line, "\"lastplayed\"") != NULL) {
			char *p = strrchr(line, '"');
			char *p0;
			if (p != NULL) {
				*p = '\0';
				p0 = strrchr(line, '"');
				if (p0 != NULL) {
					played = (unsigned)strtoul(p0 + 1, NULL, 10);
				}
			}
		}
		if (line[0] != '\0' && strchr(line, '}') != NULL && addr[0] != '\0' && section[0] != '\0') {
			uint32 flags = 0;
			if (strcmp(section, "favorites") == 0) {
				flags = k_unFavoriteFlagFavorite;
			} else if (strcmp(section, "history") == 0) {
				flags = k_unFavoriteFlagHistory;
			}
			if (flags) {
				Vellum_FavAddParsed(addr, app, played, flags);
			}
			addr[0] = '\0';
			app = 10;
			played = 0;
		}
	}
	fclose(f);
}

static void Vellum_FavLoad()
{
	char dir[512];
	char path[512];
	if (g_fav_loaded) {
		return;
	}
	g_fav_loaded = 1;
	g_fav_n = 0;
	Vellum_GameDir(dir, sizeof(dir));
#ifdef _WIN32
	_snprintf(path, sizeof(path), "%sconfig\\serverbrowser.vdf", dir);
#else
	snprintf(path, sizeof(path), "%sconfig/serverbrowser.vdf", dir);
#endif
	path[sizeof(path) - 1] = '\0';
	Vellum_FavLoadFile(path);
#ifdef _WIN32
	_snprintf(path, sizeof(path), "%splatform\\config\\ServerBrowser.vdf", dir);
#else
	snprintf(path, sizeof(path), "%splatform/config/ServerBrowser.vdf", dir);
#endif
	path[sizeof(path) - 1] = '\0';
	Vellum_FavLoadFile(path);
}

static void Vellum_FavWriteSection(FILE *f, const char *name, uint32 flag)
{
	int i;
	int n = 0;
	fprintf(f, "\t\"%s\"\n\t{\n", name);
	for (i = 0; i < g_fav_n; i++) {
		char addr[64];
		if ((g_favs[i].flags & flag) == 0) {
			continue;
		}
		n++;
		Vellum_FmtAddr(addr, sizeof(addr), g_favs[i].ip, g_favs[i].conn);
		fprintf(f, "\t\t\"%d\"\n\t\t{\n", n);
		fprintf(f, "\t\t\t\"name\"\t\t\"%s\"\n", addr);
		fprintf(f, "\t\t\t\"address\"\t\t\"%s\"\n", addr);
		fprintf(f, "\t\t\t\"lastplayed\"\t\t\"%u\"\n", (unsigned)g_favs[i].played);
		fprintf(f, "\t\t\t\"appID\"\t\t\"%u\"\n", (unsigned)g_favs[i].app);
		fprintf(f, "\t\t}\n");
	}
	fprintf(f, "\t}\n");
}

static void Vellum_FavSave()
{
	char dir[512];
	char path[512];
	FILE *f;
	Vellum_GameDir(dir, sizeof(dir));
#ifdef _WIN32
	CreateDirectoryA(dir, NULL);
	{
		char cfg[512];
		_snprintf(cfg, sizeof(cfg), "%sconfig", dir);
		CreateDirectoryA(cfg, NULL);
		_snprintf(path, sizeof(path), "%sconfig\\serverbrowser.vdf", dir);
	}
#else
	{
		char cfg[512];
		snprintf(cfg, sizeof(cfg), "%sconfig", dir);
		mkdir(cfg, 0755);
		snprintf(path, sizeof(path), "%sconfig/serverbrowser.vdf", dir);
	}
#endif
	path[sizeof(path) - 1] = '\0';
	f = fopen(path, "w");
	if (f == NULL) {
		return;
	}
	fprintf(f, "\"filters\"\n{\n");
	fprintf(f, "\t\"gamelist\"\t\t\"favorites\"\n");
	Vellum_FavWriteSection(f, "favorites", k_unFavoriteFlagFavorite);
	Vellum_FavWriteSection(f, "history", k_unFavoriteFlagHistory);
	fprintf(f, "}\n");
	fclose(f);
}

static int Vellum_FavFind(AppId_t app, uint32 ip, uint16 conn, uint16 query)
{
	int i;
	for (i = 0; i < g_fav_n; i++) {
		if (g_favs[i].app == app && g_favs[i].ip == ip && g_favs[i].conn == conn && g_favs[i].query == query) {
			return i;
		}
	}
	return -1;
}

static int Vellum_FavCount()
{
	Vellum_FavLoad();
	return g_fav_n;
}

static bool Vellum_FavGet(int i, AppId_t *app, uint32 *ip, uint16 *conn, uint16 *query, uint32 *flags, uint32 *played)
{
	Vellum_FavLoad();
	if (i < 0 || i >= g_fav_n) {
		return false;
	}
	if (app) *app = g_favs[i].app;
	if (ip) *ip = g_favs[i].ip;
	if (conn) *conn = g_favs[i].conn;
	if (query) *query = g_favs[i].query;
	if (flags) *flags = g_favs[i].flags;
	if (played) *played = g_favs[i].played;
	return true;
}

static int Vellum_FavAdd(AppId_t app, uint32 ip, uint16 conn, uint16 query, uint32 flags, uint32 played)
{
	int i;
	Vellum_FavLoad();
	i = Vellum_FavFind(app, ip, conn, query);
	if (i >= 0) {
		g_favs[i].flags |= flags;
		if (played) {
			g_favs[i].played = played;
		}
		Vellum_FavSave();
		return g_fav_n;
	}
	if (g_fav_n >= VELLUM_FAV_MAX) {
		return g_fav_n;
	}
	g_favs[g_fav_n].app = app ? app : 10;
	g_favs[g_fav_n].ip = ip;
	g_favs[g_fav_n].conn = conn;
	g_favs[g_fav_n].query = query ? query : conn;
	g_favs[g_fav_n].flags = flags ? flags : (uint32)k_unFavoriteFlagFavorite;
	g_favs[g_fav_n].played = played;
	g_fav_n++;
	Vellum_FavSave();
	return g_fav_n;
}

static bool Vellum_FavRemove(AppId_t app, uint32 ip, uint16 conn, uint16 query, uint32 flags)
{
	int i;
	Vellum_FavLoad();
	i = Vellum_FavFind(app, ip, conn, query);
	if (i < 0) {
		return false;
	}
	if (flags != 0 && (g_favs[i].flags & flags) == 0) {
		return false;
	}
	memmove(&g_favs[i], &g_favs[i + 1], (size_t)(g_fav_n - i - 1) * sizeof(g_favs[0]));
	g_fav_n--;
	Vellum_FavSave();
	return true;
}

static void Vellum_FillItem(VellumGameServerItem *it, const VellumFav *e)
{
	unsigned char *b;
	memset(it, 0, sizeof(*it));
	it->adr.conn = e->conn;
	it->adr.query = e->query;
	it->adr.ip = e->ip;
	it->hadResponse = true;
	it->appId = e->app;
	it->lastPlayed = e->played;
	it->maxPlayers = 32;
	it->ping = 0;
	strncpy(it->gameDir, "cstrike", sizeof(it->gameDir) - 1);
	b = (unsigned char *)&e->ip;
#ifdef _WIN32
	_snprintf(it->name, sizeof(it->name), "%u.%u.%u.%u:%u",
	          (unsigned)b[3], (unsigned)b[2], (unsigned)b[1], (unsigned)b[0], (unsigned)e->conn);
#else
	snprintf(it->name, sizeof(it->name), "%u.%u.%u.%u:%u",
	         (unsigned)b[3], (unsigned)b[2], (unsigned)b[1], (unsigned)b[0], (unsigned)e->conn);
#endif
	it->name[sizeof(it->name) - 1] = '\0';
}

static HServerListRequest Vellum_ListRequest(AppId_t app, uint32 flagMask, void *response)
{
	int r;
	int i;
	VellumListReq *req = NULL;
	Vellum_FavLoad();
	for (r = 0; r < VELLUM_REQ_MAX; r++) {
		if (!g_reqs[r].used) {
			req = &g_reqs[r];
			break;
		}
	}
	if (req == NULL) {
		req = &g_reqs[0];
	}
	memset(req, 0, sizeof(*req));
	req->used = 1;
	req->cb = response;
	for (i = 0; i < g_fav_n && req->count < VELLUM_FAV_MAX; i++) {
		if (app != 0 && g_favs[i].app != 0 && g_favs[i].app != app) {
			continue;
		}
		if (flagMask != 0 && (g_favs[i].flags & flagMask) == 0) {
			continue;
		}
		Vellum_FillItem(&req->items[req->count], &g_favs[i]);
		req->count++;
	}
	Vellum_Log("ListRequest count=%d mask=%u cb=%p", req->count, flagMask, response);
	return (HServerListRequest)req;
}

static void Vellum_ListRelease(HServerListRequest h)
{
	VellumListReq *req = (VellumListReq *)h;
	int r;
	if (req == NULL) {
		return;
	}
	for (r = 0; r < VELLUM_REQ_MAX; r++) {
		if (&g_reqs[r] == req) {
			req->cb = NULL;
			req->notified = 1;
			req->used = 0;
			req->count = 0;
			return;
		}
	}
}

static void *Vellum_ListDetails(HServerListRequest h, int i)
{
	VellumListReq *req = (VellumListReq *)h;
	if (req == NULL || i < 0 || i >= req->count) {
		return NULL;
	}
	return &req->items[i];
}

static int Vellum_ListCount(HServerListRequest h)
{
	VellumListReq *req = (VellumListReq *)h;
	return req != NULL ? req->count : 0;
}

static void Vellum_FlushListCallbacks()
{
	int r;
	int i;
	for (r = 0; r < VELLUM_REQ_MAX; r++) {
		VellumListReq *req = &g_reqs[r];
		SteamServerListResponse *cb;
		if (!req->used || req->notified || req->cb == NULL) {
			continue;
		}
		cb = (SteamServerListResponse *)req->cb;
		req->notified = 1;
		for (i = 0; i < req->count; i++) {
			cb->ServerResponded((HServerListRequest)req, i);
		}
		cb->RefreshComplete((HServerListRequest)req, req->count ? 0 : 2);
		Vellum_Log("ListNotify count=%d", req->count);
	}
}

static CSteamID Vellum_GameServerSteamId()
{
	return CSteamID(Vellum_GetIdentity().account_id, k_EUniversePublic, k_EAccountTypeGameServer);
}

static CSteamID Vellum_SteamIdFromAuthBlob(const void *blob, int len)
{
	if (blob != NULL && len == (int)sizeof(VellumTicket)) {
		const VellumTicket *t = (const VellumTicket *)blob;
		if (Vellum_TicketValid(t, (size_t)len)) {
			return CSteamID(t->account_id, k_EUniversePublic, k_EAccountTypeIndividual);
		}
	}
	return CSteamID(Vellum_GetIdentity().account_id ^ 0x10000u, k_EUniversePublic, k_EAccountTypeIndividual);
}

static void Vellum_QueueClientApprove(CSteamID sid)
{
	GSClientApprove_t a;
	ValidateAuthTicketResponse_t v;
	a.m_SteamID = sid;
	a.m_OwnerSteamID = sid;
	v.m_SteamID = sid;
	v.m_eAuthSessionResponse = 0;
	v.m_OwnerSteamID = sid;
	Vellum_QueueCallback(1, k_iCallback_GSClientApprove, &a, (int)sizeof(a));
	Vellum_QueueCallback(1, k_iCallback_ValidateAuthTicket, &v, (int)sizeof(v));
}

class SteamGameServerStats {
public:
	virtual SteamAPICall_t RequestUserStats(CSteamID) { return 0; }
	virtual bool GetUserStat(CSteamID, const char *, int32 *) { return false; }
	virtual bool GetUserStat(CSteamID, const char *, float *) { return false; }
	virtual bool GetUserAchievement(CSteamID, const char *, bool *got)
	{
		if (got) *got = false;
		return false;
	}
	virtual bool SetUserStat(CSteamID, const char *, int32) { return false; }
	virtual bool SetUserStat(CSteamID, const char *, float) { return false; }
	virtual bool UpdateUserAvgRateStat(CSteamID, const char *, float, double) { return false; }
	virtual bool SetUserAchievement(CSteamID, const char *) { return false; }
	virtual bool ClearUserAchievement(CSteamID, const char *) { return false; }
	virtual SteamAPICall_t StoreUserStats(CSteamID) { return 0; }
};

class SteamGameServer {
public:
	virtual bool InitGameServer(uint32, uint16, uint16, uint32, AppId_t, const char *)
	{
		g_gs_logged_on = 1;
		return true;
	}
	virtual void SetProduct(const char *) {}
	virtual void SetGameDescription(const char *) {}
	virtual void SetModDir(const char *) {}
	virtual void SetDedicatedServer(bool) {}
	virtual void LogOn(const char *)
	{
		LogOnAnonymous();
	}
	virtual void LogOnAnonymous()
	{
		GSPolicyResponse_t pol;
		g_gs_logged_on = 1;
		pol.m_bSecure = 0;
		Vellum_QueueCallback(1, k_iCallback_SteamServersConnected, NULL, 0);
		Vellum_QueueCallback(1, k_iCallback_GSPolicyResponse, &pol, (int)sizeof(pol));
	}
	virtual void LogOff() { g_gs_logged_on = 0; }
	virtual bool BLoggedOn() { return g_gs_logged_on != 0; }
	virtual bool BSecure() { return false; }
	virtual CSteamID GetSteamID() { return Vellum_GameServerSteamId(); }
	virtual bool WasRestartRequested() { return false; }
	virtual void SetMaxPlayerCount(int) {}
	virtual void SetBotPlayerCount(int) {}
	virtual void SetServerName(const char *) {}
	virtual void SetMapName(const char *) {}
	virtual void SetPasswordProtected(bool) {}
	virtual void SetSpectatorPort(uint16) {}
	virtual void SetSpectatorServerName(const char *) {}
	virtual void ClearAllKeyValues() {}
	virtual void SetKeyValue(const char *, const char *) {}
	virtual void SetGameTags(const char *) {}
	virtual void SetGameData(const char *) {}
	virtual void SetRegion(const char *) {}
	virtual void SetAdvertiseServerActive(bool) {}
	virtual HAuthTicket GetAuthSessionTicket(void *pTicket, int cbMaxTicket, uint32 *pcbTicket)
	{
		int n = Vellum_WriteAuthBlob(pTicket, cbMaxTicket);
		if (pcbTicket) *pcbTicket = (uint32)n;
		return n ? 1 : 0;
	}
	virtual int BeginAuthSession(const void *ticket, int size, CSteamID sid)
	{
		CSteamID use = sid;
		if (use.ConvertToUint64() == 0) {
			use = Vellum_SteamIdFromAuthBlob(ticket, size);
		}
		Vellum_QueueClientApprove(use);
		return 0;
	}
	virtual void EndAuthSession(CSteamID) {}
	virtual void CancelAuthTicket(HAuthTicket) {}
	virtual int UserHasLicenseForApp(CSteamID, AppId_t) { return 0; }
	virtual bool RequestUserGroupStatus(CSteamID, CSteamID) { return false; }
	virtual void GetGameplayStats() {}
	virtual SteamAPICall_t GetServerReputation() { return 0; }
	virtual SteamIPAddress_t GetPublicIP()
	{
		SteamIPAddress_t ip;
		memset(&ip, 0, sizeof(ip));
		ip.m_eType = k_ESteamIPTypeIPv4;
		return ip;
	}
	virtual bool HandleIncomingPacket(const void *, int, uint32, uint16) { return false; }
	virtual int GetNextOutgoingPacket(void *, int, uint32 *, uint16 *) { return 0; }
	virtual SteamAPICall_t AssociateWithClan(CSteamID) { return 0; }
	virtual SteamAPICall_t ComputeNewPlayerCompatibility(CSteamID) { return 0; }
	virtual bool SendUserConnectAndAuthenticate(uint32, const void *blob, uint32 size, CSteamID *outId)
	{
		CSteamID sid = Vellum_SteamIdFromAuthBlob(blob, (int)size);
		if (outId) *outId = sid;
		Vellum_QueueClientApprove(sid);
		return true;
	}
	virtual CSteamID CreateUnauthenticatedUserConnection()
	{
		return CSteamID(++g_gs_bots | 0x70000000u, k_EUniversePublic, k_EAccountTypeIndividual);
	}
	virtual void SendUserDisconnect(CSteamID) {}
	virtual bool BUpdateUserData(CSteamID, const char *, uint32) { return true; }
	virtual void SetMasterServerHeartbeatInterval_DEPRECATED(int) {}
	virtual void ForceMasterServerHeartbeat_DEPRECATED() {}
};

static SteamGameServer g_gameserver;
static SteamGameServerStats g_gsstats;

class SteamClient {
public:
	virtual HSteamPipe CreateSteamPipe() { return 1; }
	virtual bool BReleaseSteamPipe(HSteamPipe) { return true; }
	virtual HSteamUser ConnectToGlobalUser(HSteamPipe) { return 1; }
	virtual HSteamUser CreateLocalUser(HSteamPipe *phSteamPipe, int)
	{
		if (phSteamPipe) *phSteamPipe = 1;
		return 1;
	}
	virtual void ReleaseUser(HSteamPipe, HSteamUser) {}
	virtual void *GetISteamUser(HSteamUser, HSteamPipe, const char *ver) { return Vellum_PickUser(ver); }
	virtual void *GetISteamGameServer(HSteamUser, HSteamPipe, const char *) { return &g_gameserver; }
	virtual void SetLocalIPBinding(uint32, uint16) {}
	virtual void *GetISteamFriends(HSteamUser, HSteamPipe, const char *ver) { return Vellum_PickFriends(ver); }
	virtual void *GetISteamUtils(HSteamPipe, const char *) { return &g_utils; }
	virtual void *GetISteamMatchmaking(HSteamUser, HSteamPipe, const char *) { return &g_mm; }
	virtual void *GetISteamMatchmakingServers(HSteamUser, HSteamPipe, const char *) { return &g_mms; }
	virtual void *GetISteamGenericInterface(HSteamUser user, HSteamPipe pipe, const char *ver);
	virtual void *GetISteamUserStats(HSteamUser, HSteamPipe, const char *) { return &g_stats; }
	virtual void *GetISteamGameServerStats(HSteamUser, HSteamPipe, const char *) { return &g_gsstats; }
	virtual void *GetISteamApps(HSteamUser, HSteamPipe, const char *) { return &g_apps; }
	virtual void *GetISteamNetworking(HSteamUser, HSteamPipe, const char *) { return &g_net; }
	virtual void *GetISteamRemoteStorage(HSteamUser, HSteamPipe, const char *) { return &g_remote; }
	virtual void *GetISteamScreenshots(HSteamUser, HSteamPipe, const char *) { return &g_shots; }
	virtual void RunFrame() { Vellum_FlushListCallbacks(); }
	virtual uint32 GetIPCCallCount() { return 0; }
	virtual void SetWarningMessageHook(SteamAPIWarningMessageHook_t) {}
	virtual bool BShutdownIfAllPipesClosed() { return true; }
	virtual void *GetISteamHTTP(HSteamUser, HSteamPipe, const char *) { return &g_http; }
	virtual void *GetISteamUnifiedMessages(HSteamUser, HSteamPipe, const char *) { return &g_unified; }
	virtual void *GetISteamController(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamUGC(HSteamUser, HSteamPipe, const char *) { return NULL; }
};

class SteamClient017 : public SteamClient {
public:
	virtual void *GetISteamAppList(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamMusic(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamMusicRemote(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamHTMLSurface(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess(void (*)()) {}
	virtual void DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess(void (*)()) {}
	virtual void Set_SteamAPI_CCheckCallbackRegisteredInProcess(void *) {}
	virtual void *GetISteamInventory(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamVideo(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamParentalSettings(HSteamUser, HSteamPipe, const char *) { return NULL; }
};

class SteamClient020 {
public:
	virtual HSteamPipe CreateSteamPipe() { return 1; }
	virtual bool BReleaseSteamPipe(HSteamPipe) { return true; }
	virtual HSteamUser ConnectToGlobalUser(HSteamPipe) { return 1; }
	virtual HSteamUser CreateLocalUser(HSteamPipe *phSteamPipe, int)
	{
		if (phSteamPipe) *phSteamPipe = 1;
		return 1;
	}
	virtual void ReleaseUser(HSteamPipe, HSteamUser) {}
	virtual void *GetISteamUser(HSteamUser, HSteamPipe, const char *ver)
	{
		Vellum_Log("GetISteamUser020 %s", ver ? ver : "(null)");
		return &g_user023;
	}
	virtual void *GetISteamGameServer(HSteamUser, HSteamPipe, const char *) { return &g_gameserver; }
	virtual void SetLocalIPBinding(const void *, uint16) {}
	virtual void *GetISteamFriends(HSteamUser, HSteamPipe, const char *) { return &g_friends017; }
	virtual void *GetISteamUtils(HSteamPipe, const char *) { return &g_utils; }
	virtual void *GetISteamMatchmaking(HSteamUser, HSteamPipe, const char *) { return &g_mm; }
	virtual void *GetISteamMatchmakingServers(HSteamUser, HSteamPipe, const char *) { return &g_mms; }
	virtual void *GetISteamGenericInterface(HSteamUser user, HSteamPipe pipe, const char *ver);
	virtual void *GetISteamUserStats(HSteamUser, HSteamPipe, const char *) { return &g_stats; }
	virtual void *GetISteamGameServerStats(HSteamUser, HSteamPipe, const char *) { return &g_gsstats; }
	virtual void *GetISteamApps(HSteamUser, HSteamPipe, const char *) { return &g_apps; }
	virtual void *GetISteamNetworking(HSteamUser, HSteamPipe, const char *) { return &g_net; }
	virtual void *GetISteamRemoteStorage(HSteamUser, HSteamPipe, const char *) { return &g_remote; }
	virtual void *GetISteamScreenshots(HSteamUser, HSteamPipe, const char *) { return &g_shots; }
	virtual void *GetISteamGameSearch(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void RunFrame() { Vellum_FlushListCallbacks(); }
	virtual uint32 GetIPCCallCount() { return 0; }
	virtual void SetWarningMessageHook(SteamAPIWarningMessageHook_t) {}
	virtual bool BShutdownIfAllPipesClosed() { return true; }
	virtual void *GetISteamHTTP(HSteamUser, HSteamPipe, const char *) { return &g_http; }
	virtual void *DEPRECATED_GetISteamUnifiedMessages(HSteamUser, HSteamPipe, const char *) { return &g_unified; }
	virtual void *GetISteamController(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamUGC(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamAppList(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamMusic(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamMusicRemote(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamHTMLSurface(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void DEPRECATED_Set_SteamAPI_CPostAPIResultInProcess(void (*)()) {}
	virtual void DEPRECATED_Remove_SteamAPI_CPostAPIResultInProcess(void (*)()) {}
	virtual void Set_SteamAPI_CCheckCallbackRegisteredInProcess(void *) {}
	virtual void *GetISteamInventory(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamVideo(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamParentalSettings(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamInput(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamParties(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamRemotePlay(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void DestroyAllInterfaces() {}
};

void *SteamClient::GetISteamGenericInterface(HSteamUser user, HSteamPipe pipe, const char *ver)
{
	if (ver == NULL) {
		return NULL;
	}
	Vellum_Log("GenericInterface %s", ver);
	if (strncmp(ver, "SteamUser", 9) == 0) return GetISteamUser(user, pipe, ver);
	if (strncmp(ver, "SteamFriends", 12) == 0) return GetISteamFriends(user, pipe, ver);
	if (strncmp(ver, "SteamUtils", 10) == 0) return GetISteamUtils(pipe, ver);
	if (strncmp(ver, "SteamMatchMakingServers", 23) == 0) return GetISteamMatchmakingServers(user, pipe, ver);
	if (strncmp(ver, "SteamMatchMaking", 16) == 0) return GetISteamMatchmaking(user, pipe, ver);
	if (strncmp(ver, "STEAMUSERSTATS", 14) == 0) return GetISteamUserStats(user, pipe, ver);
	if (strncmp(ver, "STEAMAPPS", 9) == 0) return GetISteamApps(user, pipe, ver);
	if (strncmp(ver, "SteamNetworkingSockets", 22) == 0) return &g_netsockets;
	if (strncmp(ver, "SteamNetworkingUtils", 20) == 0) return &g_netutils;
	if (strncmp(ver, "SteamNetworking", 15) == 0) return GetISteamNetworking(user, pipe, ver);
	if (strncmp(ver, "STEAMREMOTESTORAGE", 18) == 0) return GetISteamRemoteStorage(user, pipe, ver);
	if (strncmp(ver, "STEAMSCREENSHOTS", 16) == 0) return GetISteamScreenshots(user, pipe, ver);
	if (strncmp(ver, "STEAMHTTP", 9) == 0) return GetISteamHTTP(user, pipe, ver);
	if (strncmp(ver, "STEAMUNIFIEDMESSAGES", 20) == 0) return GetISteamUnifiedMessages(user, pipe, ver);
	if (strncmp(ver, "SteamGameServerStats", 20) == 0) return GetISteamGameServerStats(user, pipe, ver);
	if (strncmp(ver, "SteamGameServer", 15) == 0) return GetISteamGameServer(user, pipe, ver);
	return NULL;
}

void *SteamClient020::GetISteamGenericInterface(HSteamUser user, HSteamPipe pipe, const char *ver)
{
	if (ver == NULL) {
		return NULL;
	}
	if (strncmp(ver, "SteamController", 15) == 0 || strncmp(ver, "SteamInput", 10) == 0) {
		return NULL;
	}
	Vellum_Log("GenericInterface020 %s", ver);
	if (strncmp(ver, "SteamUser", 9) == 0) return GetISteamUser(user, pipe, ver);
	if (strncmp(ver, "SteamFriends", 12) == 0) return GetISteamFriends(user, pipe, ver);
	if (strncmp(ver, "SteamUtils", 10) == 0) return GetISteamUtils(pipe, ver);
	if (strncmp(ver, "SteamMatchMakingServers", 23) == 0) return GetISteamMatchmakingServers(user, pipe, ver);
	if (strncmp(ver, "SteamMatchMaking", 16) == 0) return GetISteamMatchmaking(user, pipe, ver);
	if (strncmp(ver, "STEAMUSERSTATS", 14) == 0) return GetISteamUserStats(user, pipe, ver);
	if (strncmp(ver, "STEAMAPPS", 9) == 0) return GetISteamApps(user, pipe, ver);
	if (strncmp(ver, "SteamNetworkingSockets", 22) == 0) return &g_netsockets;
	if (strncmp(ver, "SteamNetworkingUtils", 20) == 0) return &g_netutils;
	if (strncmp(ver, "SteamNetworking", 15) == 0) return GetISteamNetworking(user, pipe, ver);
	if (strncmp(ver, "STEAMREMOTESTORAGE", 18) == 0) return GetISteamRemoteStorage(user, pipe, ver);
	if (strncmp(ver, "STEAMSCREENSHOTS", 16) == 0) return GetISteamScreenshots(user, pipe, ver);
	if (strncmp(ver, "STEAMHTTP", 9) == 0) return GetISteamHTTP(user, pipe, ver);
	if (strncmp(ver, "STEAMUNIFIEDMESSAGES", 20) == 0) return DEPRECATED_GetISteamUnifiedMessages(user, pipe, ver);
	if (strncmp(ver, "SteamGameServerStats", 20) == 0) return GetISteamGameServerStats(user, pipe, ver);
	if (strncmp(ver, "SteamGameServer", 15) == 0) return GetISteamGameServer(user, pipe, ver);
	return NULL;
}

static SteamClient g_client;
static SteamClient017 g_client017;
static SteamClient020 g_client020;

STEAM_EXPORT void *STEAM_CALL CreateInterface(const char *pName, int *pReturnCode)
{
	Vellum_InitIdentity();
	Vellum_InstallCrashLog();
	Vellum_Log("CreateInterface %s", pName ? pName : "(null)");
	if (pName != NULL && (strcmp(pName, "SteamClient012") == 0 || strcmp(pName, "SteamClient011") == 0)) {
		if (pReturnCode) *pReturnCode = 0;
		return &g_client;
	}
	if (pName != NULL && strcmp(pName, "SteamClient017") == 0) {
		if (pReturnCode) *pReturnCode = 0;
		return &g_client017;
	}
	if (pName != NULL && strcmp(pName, "SteamClient020") == 0) {
		if (pReturnCode) *pReturnCode = 0;
		return &g_client020;
	}
	if (pName != NULL && (strcmp(pName, "SteamGameServer014") == 0 || strcmp(pName, "SteamGameServer015") == 0)) {
		if (pReturnCode) *pReturnCode = 0;
		return &g_gameserver;
	}
	if (pName != NULL && strcmp(pName, "SteamGameServerStats001") == 0) {
		if (pReturnCode) *pReturnCode = 0;
		return &g_gsstats;
	}
	if (pName != NULL && strncmp(pName, "SteamNetworkingSockets", 22) == 0) {
		if (pReturnCode) *pReturnCode = 0;
		return &g_netsockets;
	}
	if (pName != NULL && strncmp(pName, "SteamNetworkingUtils", 20) == 0) {
		if (pReturnCode) *pReturnCode = 0;
		return &g_netutils;
	}
	if (pReturnCode) *pReturnCode = 1;
	return NULL;
}

STEAM_EXPORT void *STEAM_CALL SteamInternal_CreateInterface(const char *pName)
{
	return CreateInterface(pName, NULL);
}

STEAM_EXPORT bool STEAM_CALL Steam_BGetCallback(HSteamPipe, CallbackMsg_t *pCallback)
{
	Vellum_FlushListCallbacks();
	if (pCallback == NULL || g_cbq_count <= 0) {
		return false;
	}
	g_cb_last = g_cbq[g_cbq_head];
	g_cbq_head = (g_cbq_head + 1) % VELLUM_CB_QUEUE;
	g_cbq_count--;
	g_cb_have_last = 1;
	pCallback->m_hSteamUser = g_cb_last.user;
	pCallback->m_iCallback = g_cb_last.id;
	pCallback->m_pubParam = g_cb_last.data;
	pCallback->m_cubParam = g_cb_last.size;
	Vellum_Log("BGetCallback id=%d size=%d", g_cb_last.id, g_cb_last.size);
	return true;
}

STEAM_EXPORT void STEAM_CALL Steam_FreeLastCallback(HSteamPipe)
{
	g_cb_have_last = 0;
}

STEAM_EXPORT bool STEAM_CALL Steam_GetAPICallResult(HSteamPipe, SteamAPICall_t, void *, int, int, bool *)
{
	return false;
}
