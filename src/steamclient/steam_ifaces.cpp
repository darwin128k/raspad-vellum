#include "types.h"
#include "identity.h"

#include <string.h>
#include <time.h>

/* Thin Steamworks facades for GoldSrc steam_api (SteamClient012 era).
 * Methods that the engine actually needs are real; the rest are no-ops with
 * the correct vtable slots so SteamAPI_Init can obtain every interface. */

class SteamUser {
public:
	virtual HSteamUser GetHSteamUser() { return 1; }
	virtual bool BLoggedOn() { return true; }
	virtual CSteamID GetSteamID() { return Vellum_GetIdentity().steam_id; }
	virtual int InitiateGameConnection(void *pAuthBlob, int cbMaxAuthBlob, CSteamID, uint32, uint16, bool)
	{
		return Vellum_WriteAuthBlob(pAuthBlob, cbMaxAuthBlob);
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
		int n = Vellum_WriteAuthBlob(pTicket, cbMaxTicket);
		if (pcbTicket) *pcbTicket = (uint32)n;
		return n ? 1 : 0;
	}
	virtual int BeginAuthSession(const void *, int, CSteamID) { return 0; }
	virtual void EndAuthSession(CSteamID) {}
	virtual void CancelAuthTicket(HAuthTicket) {}
	virtual int UserHasLicenseForApp(CSteamID, AppId_t) { return 0; }
	virtual bool BIsBehindNAT() { return false; }
	virtual void AdvertiseGame(CSteamID, uint32, uint16) {}
	virtual SteamAPICall_t RequestEncryptedAppTicket(void *, int) { return 0; }
	virtual bool GetEncryptedAppTicket(void *, int, uint32 *) { return false; }
	virtual int GetGameBadgeLevel(int, bool) { return 0; }
	virtual int GetPlayerSteamLevel() { return 1; }
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
	virtual void RunFrame() {}
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
};

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
};

class SteamMatchmaking {
public:
	virtual int GetFavoriteGameCount() { return 0; }
	virtual bool GetFavoriteGame(int, AppId_t *, uint32 *, uint16 *, uint16 *, uint32 *, uint32 *) { return false; }
	virtual int AddFavoriteGame(AppId_t, uint32, uint16, uint16, uint32, uint32) { return 0; }
	virtual bool RemoveFavoriteGame(AppId_t, uint32, uint16, uint16, uint32) { return false; }
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
	virtual HServerListRequest RequestFavoritesServerList(AppId_t, void **, uint32, void *) { return NULL; }
	virtual HServerListRequest RequestHistoryServerList(AppId_t, void **, uint32, void *) { return NULL; }
	virtual HServerListRequest RequestSpectatorServerList(AppId_t, void **, uint32, void *) { return NULL; }
	virtual void ReleaseRequest(HServerListRequest) {}
	virtual void *GetServerDetails(HServerListRequest, int) { return NULL; }
	virtual void CancelQuery(HServerListRequest) {}
	virtual void RefreshQuery(HServerListRequest) {}
	virtual bool IsRefreshing(HServerListRequest) { return false; }
	virtual int GetServerCount(HServerListRequest) { return 0; }
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
static SteamFriends g_friends;
static SteamUtils g_utils;
static SteamApps g_apps;
static SteamHTTP g_http;
static SteamMatchmaking g_mm;
static SteamMatchmakingServers g_mms;
static SteamUserStats g_stats;
static SteamNetworking g_net;
static SteamRemoteStorage g_remote;
static SteamScreenshots g_shots;
static SteamUnifiedMessages g_unified;

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
	virtual void *GetISteamUser(HSteamUser, HSteamPipe, const char *) { return &g_user; }
	virtual void *GetISteamGameServer(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void SetLocalIPBinding(uint32, uint16) {}
	virtual void *GetISteamFriends(HSteamUser, HSteamPipe, const char *) { return &g_friends; }
	virtual void *GetISteamUtils(HSteamPipe, const char *) { return &g_utils; }
	virtual void *GetISteamMatchmaking(HSteamUser, HSteamPipe, const char *) { return &g_mm; }
	virtual void *GetISteamMatchmakingServers(HSteamUser, HSteamPipe, const char *) { return &g_mms; }
	virtual void *GetISteamGenericInterface(HSteamUser user, HSteamPipe pipe, const char *ver);
	virtual void *GetISteamUserStats(HSteamUser, HSteamPipe, const char *) { return &g_stats; }
	virtual void *GetISteamGameServerStats(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamApps(HSteamUser, HSteamPipe, const char *) { return &g_apps; }
	virtual void *GetISteamNetworking(HSteamUser, HSteamPipe, const char *) { return &g_net; }
	virtual void *GetISteamRemoteStorage(HSteamUser, HSteamPipe, const char *) { return &g_remote; }
	virtual void *GetISteamScreenshots(HSteamUser, HSteamPipe, const char *) { return &g_shots; }
	virtual void RunFrame() {}
	virtual uint32 GetIPCCallCount() { return 0; }
	virtual void SetWarningMessageHook(SteamAPIWarningMessageHook_t) {}
	virtual bool BShutdownIfAllPipesClosed() { return true; }
	virtual void *GetISteamHTTP(HSteamUser, HSteamPipe, const char *) { return &g_http; }
	virtual void *GetISteamUnifiedMessages(HSteamUser, HSteamPipe, const char *) { return &g_unified; }
	virtual void *GetISteamController(HSteamUser, HSteamPipe, const char *) { return NULL; }
	virtual void *GetISteamUGC(HSteamUser, HSteamPipe, const char *) { return NULL; }
};

void *SteamClient::GetISteamGenericInterface(HSteamUser user, HSteamPipe pipe, const char *ver)
{
	if (ver == NULL) {
		return NULL;
	}
	if (strncmp(ver, "SteamUser", 9) == 0) return GetISteamUser(user, pipe, ver);
	if (strncmp(ver, "SteamFriends", 12) == 0) return GetISteamFriends(user, pipe, ver);
	if (strncmp(ver, "SteamUtils", 10) == 0) return GetISteamUtils(pipe, ver);
	if (strncmp(ver, "SteamMatchMakingServers", 23) == 0) return GetISteamMatchmakingServers(user, pipe, ver);
	if (strncmp(ver, "SteamMatchMaking", 16) == 0) return GetISteamMatchmaking(user, pipe, ver);
	if (strncmp(ver, "STEAMUSERSTATS", 14) == 0) return GetISteamUserStats(user, pipe, ver);
	if (strncmp(ver, "STEAMAPPS", 9) == 0) return GetISteamApps(user, pipe, ver);
	if (strncmp(ver, "SteamNetworking", 15) == 0) return GetISteamNetworking(user, pipe, ver);
	if (strncmp(ver, "STEAMREMOTESTORAGE", 18) == 0) return GetISteamRemoteStorage(user, pipe, ver);
	if (strncmp(ver, "STEAMSCREENSHOTS", 16) == 0) return GetISteamScreenshots(user, pipe, ver);
	if (strncmp(ver, "STEAMHTTP", 9) == 0) return GetISteamHTTP(user, pipe, ver);
	if (strncmp(ver, "STEAMUNIFIEDMESSAGES", 20) == 0) return GetISteamUnifiedMessages(user, pipe, ver);
	return NULL;
}

static SteamClient g_client;

extern "C" {

__declspec(dllexport) void *__cdecl CreateInterface(const char *pName, int *pReturnCode)
{
	Vellum_InitIdentity();
	if (pName != NULL && (strcmp(pName, "SteamClient012") == 0 || strcmp(pName, "SteamClient011") == 0)) {
		if (pReturnCode) *pReturnCode = 0;
		return &g_client;
	}
	if (pReturnCode) *pReturnCode = 1;
	return NULL;
}

__declspec(dllexport) bool __cdecl Steam_BGetCallback(HSteamPipe, CallbackMsg_t *)
{
	return false;
}

__declspec(dllexport) void __cdecl Steam_FreeLastCallback(HSteamPipe) {}

__declspec(dllexport) bool __cdecl Steam_GetAPICallResult(HSteamPipe, SteamAPICall_t, void *, int, int, bool *)
{
	return false;
}

} /* extern "C" */
