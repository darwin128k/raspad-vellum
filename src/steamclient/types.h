#pragma once

#include <stdint.h>

typedef uint8_t  uint8;
typedef int32_t  int32;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int64_t  int64;
typedef uint16_t uint16;
typedef int32_t  HSteamPipe;
typedef int32_t  HSteamUser;
typedef uint32_t AppId_t;
typedef uint32_t DepotId_t;
typedef uint32_t HAuthTicket;
typedef uint64_t SteamAPICall_t;
typedef uint32_t HTTPRequestHandle;
typedef uint32_t ScreenshotHandle;
typedef uint32_t SNetListenSocket_t;
typedef uint32_t SNetSocket_t;
typedef uint64_t UGCHandle_t;
typedef uint64_t PublishedFileId_t;
typedef uint64_t GID_t;
typedef uint64_t JobID_t;
typedef uint64_t SteamLeaderboard_t;
typedef uint64_t SteamLeaderboardEntries_t;
typedef void    *HServerListRequest;
typedef int      HServerQuery;

typedef void (*SteamAPIWarningMessageHook_t)(int, const char *);

enum { k_EUniversePublic = 1 };
enum { k_EAccountTypeIndividual = 1 };
enum { k_EAccountTypeGameServer = 3 };

class CSteamID {
public:
	CSteamID() : m_unAll64Bits(0) {}
	explicit CSteamID(uint64 v) : m_unAll64Bits(v) {}
	CSteamID(uint32 accountId, uint32 universe, uint32 type)
	{
		m_unAll64Bits = (uint64)accountId
			| ((uint64)1 << 32)
			| ((uint64)type << 52)
			| ((uint64)universe << 56);
	}
	uint64 ConvertToUint64() const { return m_unAll64Bits; }
	uint32 GetAccountID() const { return (uint32)m_unAll64Bits; }
	uint64 m_unAll64Bits;
};

class CGameID {
public:
	CGameID() : m_ulGameID(0) {}
	explicit CGameID(uint64 v) : m_ulGameID(v) {}
	uint64 m_ulGameID;
};

struct CallbackMsg_t {
	HSteamUser m_hSteamUser;
	int m_iCallback;
	uint8 *m_pubParam;
	int m_cubParam;
};

struct FriendGameInfo_t {
	CGameID m_gameID;
	uint32 m_unGameIP;
	uint16 m_usGamePort;
	uint16 m_usQueryPort;
	CSteamID m_steamIDLobby;
};

struct P2PSessionState_t {
	uint8 m_bConnectionActive;
	uint8 m_bConnecting;
	uint8 m_eP2PSessionError;
	uint8 m_bUsingRelay;
	int32 m_nBytesQueuedForSend;
	int32 m_nPacketsQueuedForSend;
	uint32 m_nRemoteIP;
	uint16 m_nRemotePort;
};

struct LeaderboardEntry_t {
	CSteamID m_steamIDUser;
	int32 m_nGlobalRank;
	int32 m_nScore;
	int32 m_cDetails;
	UGCHandle_t m_hUGC;
};

struct SteamParamStringArray_t {
	const char **m_ppStrings;
	int32 m_nNumStrings;
};
