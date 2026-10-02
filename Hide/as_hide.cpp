#include <stdio.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "as_hide.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include "schemasystem/schematypes.h"
#include "filesystem.h"

Hide g_Hide;
PLUGIN_EXPOSE(Hide, g_Hide);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;

IUtilsApi* g_pUtils = nullptr;
IPlayersApi* g_pPlayers = nullptr;
IAdminApi* g_pAdmin = nullptr;

#define MAX_SLOTS 64
#define TEAM_NONE 0
#define TEAM_SPECTATOR 1
#define TEAM_T 2
#define TEAM_CT 3

// Exposed by Utils since 1.9.1 - the release that added SetTransmitState listeners and
// pre/post event hooks. Asking for it is safe on any version, unlike calling a method
// an old Utils does not have.
#define LAYOUT_INTERFACE_NAME "ILayoutApi"

// A jointeam this soon after player_connect_full is the client re-joining its old team
// by itself (map change), not a click in the team menu
#define AUTO_JOIN_SECONDS 5.0

typedef std::chrono::steady_clock Clock;

struct HideConfig
{
	// "permission" as written in the config (the admin menu item takes it as is) and split by |
	std::string sPermission = "@admin/hide";
	std::vector<std::string> vPermissions = {"@admin/hide"};
	std::string sAutoHidePermission;
	std::vector<std::string> vChatCommands = {"!hide", "/hide"};
	std::vector<std::string> vConsoleCommands = {"mm_hide"};
	std::vector<std::string> vListChatCommands = {"!hidelist"};
	std::vector<std::string> vListConsoleCommands = {"mm_hidelist"};
	bool bMenuItem = true;
	std::string sMenuCategory = "server";
	std::string sMenuCategoryName = "Category_Server";
	bool bHidePawn = true;
	bool bSilentTeamChange = true;
	bool bSilentDeath = true;
	bool bSilentConnect = true;
	bool bSilentDisconnect = true;
	bool bBlockChat = true;
	bool bBlockVoice = true;
	bool bRehideOnSpectator = true;
	bool bRehideOnTeamChange = true;
	bool bBlockAutoTeam = true;
	bool bKeepHidden = true;
	bool bAutoHideOnSpectate = false;
	std::string sTeamMenuRestore = "0";
};

HideConfig g_Config;
std::map<std::string, std::string> g_mPhrases;

bool g_bHidden[MAX_SLOTS];
// The hide sequence (suicide -> spectators -> unassigned) is running for the slot
bool g_bApplying[MAX_SLOTS];
// Team change events of the slot are not broadcast until this moment (used when leaving hide mode)
Clock::time_point g_tSilentUntil[MAX_SLOTS];
// player_connect_full of the current connection / map
Clock::time_point g_tConnectedAt[MAX_SLOTS];
// The player picked a team in the team menu himself since connecting
bool g_bJoinedByPlayer[MAX_SLOTS];
// Hidden admin put into a team by the server / other plugins: limited number of re-hides
int g_iCorrections[MAX_SLOTS];
Clock::time_point g_tFirstCorrection[MAX_SLOTS];
Clock::time_point g_tVoiceNotified[MAX_SLOTS];
// Bumped on every (re)connect: cancels a restore that is still waiting for the old connection
int g_iRestoreToken[MAX_SLOTS];
// SteamID64 of admins that get hide mode back after a map change / reconnect (until server restart)
std::unordered_set<uint64> g_setKeepHidden;
// Admins with auto_hide_permission that turned hide mode off this session
std::unordered_set<uint64> g_setAutoHideOptOut;

// Utils 1.9.1+: SetTransmitState, entity listeners and pre/post event hooks are available
bool g_bExtendedApi = false;

std::set<CTimer*> g_setTimers;

int g_iTeamMenuLocks = 0;

// entity index -> slot of the hidden admin it belongs to
std::map<int, int> g_mTransmitHidden;

// The admin core keeps raw pointers to these, so they are allocated once and never freed
// (string literals would dangle after this module is unloaded)
bool g_bMenuRegistered = false;
const char* g_szMenuIdentity = nullptr;
const char* g_szMenuName = nullptr;
const char* g_szMenuCategory = nullptr;
const char* g_szMenuCategoryName = nullptr;
const char* g_szMenuFlags = nullptr;

class HideApi : public IHideApi
{
public:
	bool IsClientHidden(int iSlot) override
	{
		return iSlot >= 0 && iSlot < MAX_SLOTS && g_bHidden[iSlot];
	}
};

HideApi g_HideApi;

class HideEntityListener : public IUtilsEntityListener
{
public:
	void CheckTransmit(CCheckTransmitInfo **pInfoInfoList, int nInfoCount, CBitVec<16384> &unionTransmitEdicts, CBitVec<16384> &, const Entity2Networkable_t **pNetworkables, const uint16 *pEntityIndicies, int nEntityIndices, bool bEnablePVSBits) override;
};

HideEntityListener g_EntityListener;

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils->GetCGameEntitySystem();
}

void StartupServer()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
}

///////////////////////////////////////
// Engine access in one place

void RunServerCommand(const char* szCommand)
{
	engine->ServerCommand(szCommand);
}

CEntityInstance* GetEntityByIndex(int iIndex)
{
	return g_pEntitySystem ? g_pEntitySystem->GetEntityInstance(CEntityIndex(iIndex)) : nullptr;
}

CEntityInstance* GetEntityByHandle(const CEntityHandle& hEntity)
{
	return g_pEntitySystem ? g_pEntitySystem->GetEntityInstance(hEntity) : nullptr;
}

int GetEntityIndex(CEntityInstance* pEntity)
{
	return pEntity->GetEntityIndex().Get();
}

const char* GetEntityClassname(CEntityInstance* pEntity)
{
	return pEntity->GetClassname();
}

bool LoadKeyValuesFile(KeyValues& kv, const char* pszPath)
{
	return kv.LoadFromFile(g_pFullFileSystem, pszPath);
}

bool IsValidSlot(int iSlot)
{
	return iSlot >= 0 && iSlot < MAX_SLOTS;
}

double SecondsSince(Clock::time_point tPoint)
{
	return std::chrono::duration<double>(Clock::now() - tPoint).count();
}

std::vector<std::string> SplitString(const std::string& szString, char cDelimiter)
{
	std::vector<std::string> vecResult;
	size_t iStart = 0;
	while (iStart <= szString.size())
	{
		size_t iEnd = szString.find(cDelimiter, iStart);
		if (iEnd == std::string::npos) iEnd = szString.size();
		std::string sToken = szString.substr(iStart, iEnd - iStart);
		size_t iFirst = sToken.find_first_not_of(" \t");
		size_t iLast = sToken.find_last_not_of(" \t");
		if (iFirst != std::string::npos) vecResult.push_back(sToken.substr(iFirst, iLast - iFirst + 1));
		iStart = iEnd + 1;
	}
	return vecResult;
}

///////////////////////////////////////
// Entity fields, resolved through the schema system of the running game

#ifdef _WIN32
#define SERVER_MODULE "server.dll"
#else
#define SERVER_MODULE "libserver.so"
#endif

int FindSchemaOffset(CSchemaClassInfo* pClass, const char* szField)
{
	for (int i = 0; i < pClass->m_nFieldCount; i++)
	{
		if (!strcmp(pClass->m_pFields[i].m_pszName, szField)) return pClass->m_pFields[i].m_nSingleInheritanceOffset;
	}
	for (int i = 0; i < pClass->m_nBaseClassCount; i++)
	{
		CSchemaClassInfo* pBase = pClass->m_pBaseClasses[i].m_pClass;
		if (!pBase) continue;
		int iOffset = FindSchemaOffset(pBase, szField);
		if (iOffset >= 0) return iOffset + pClass->m_pBaseClasses[i].m_nOffset;
	}
	return -1;
}

struct SchemaField
{
	const char* szClass;
	const char* szField;
	// -2: not resolved yet, -1: not found
	int iOffset;

	int Offset()
	{
		if (iOffset != -2) return iOffset;
		CSchemaSystemTypeScope* pScope = g_pSchemaSystem->FindTypeScopeForModule(SERVER_MODULE);
		// the server module is not loaded yet: try again next time
		if (!pScope) return -1;
		CSchemaClassInfo* pClass = pScope->FindDeclaredClass(szClass).Get();
		iOffset = pClass ? FindSchemaOffset(pClass, szField) : -1;
		if (iOffset < 0) g_pUtils->ErrorLog("[%s] Schema field %s::%s not found", g_PLAPI->GetLogTag(), szClass, szField);
		return iOffset;
	}
};

SchemaField g_fTeamNum = {"CBaseEntity", "m_iTeamNum", -2};
SchemaField g_fLifeState = {"CBaseEntity", "m_lifeState", -2};
SchemaField g_fSteamID = {"CBasePlayerController", "m_steamID", -2};
// the player model (CCSPlayerPawn)
SchemaField g_fPlayerPawn = {"CCSPlayerController", "m_hPlayerPawn", -2};
// free camera / spectating pawn (CCSObserverPawn), holds the observer target
SchemaField g_fObserverPawn = {"CCSPlayerController", "m_hObserverPawn", -2};
SchemaField g_fForceTeamTime = {"CCSPlayerController", "m_flForceTeamTime", -2};

template<typename T>
bool ReadField(CEntityInstance* pEntity, SchemaField& field, T& value)
{
	if (!pEntity) return false;
	int iOffset = field.Offset();
	if (iOffset < 0) return false;
	value = *reinterpret_cast<T*>(reinterpret_cast<uint8*>(pEntity) + iOffset);
	return true;
}

template<typename T>
bool WriteField(CEntityInstance* pEntity, SchemaField& field, const T& value)
{
	if (!pEntity) return false;
	int iOffset = field.Offset();
	if (iOffset < 0) return false;
	*reinterpret_cast<T*>(reinterpret_cast<uint8*>(pEntity) + iOffset) = value;
	g_pUtils->SetStateChanged(reinterpret_cast<CBaseEntity*>(pEntity), field.szClass, field.szField);
	return true;
}

CEntityInstance* GetController(int iSlot)
{
	if (!IsValidSlot(iSlot)) return nullptr;
	CEntityInstance* pController = GetEntityByIndex(iSlot + 1);
	if (!pController || strcmp(GetEntityClassname(pController), "cs_player_controller")) return nullptr;
	return pController;
}

CEntityInstance* GetHandleField(CEntityInstance* pEntity, SchemaField& field)
{
	CEntityHandle hEntity;
	if (!ReadField(pEntity, field, hEntity) || !hEntity.IsValid()) return nullptr;
	return GetEntityByHandle(hEntity);
}

int GetTeam(CEntityInstance* pEntity)
{
	uint8 iTeam;
	return ReadField(pEntity, g_fTeamNum, iTeam) ? iTeam : -1;
}

bool IsAlive(CEntityInstance* pEntity)
{
	uint8 iLifeState;
	return ReadField(pEntity, g_fLifeState, iLifeState) && iLifeState == LIFE_ALIVE;
}

// Utils knows the SteamID only after Steam authorization; the controller has it right after connecting
uint64 GetSlotSteamID(int iSlot)
{
	uint64 iSteamID64 = g_pPlayers->GetSteamID64(iSlot);
	if (!iSteamID64) ReadField(GetController(iSlot), g_fSteamID, iSteamID64);
	return iSteamID64;
}

///////////////////////////////////////
// Timers (tracked so they can be removed when the module is unloaded)

// fn returns true to run again after flInterval
void Repeat(float flInterval, std::function<bool()> fn)
{
	auto pHolder = std::make_shared<CTimer*>(nullptr);
	*pHolder = g_pUtils->CreateTimer(flInterval, [pHolder, fn, flInterval]() {
		if (fn()) return flInterval;
		g_setTimers.erase(*pHolder);
		return -1.0f;
	});
	g_setTimers.insert(*pHolder);
}

void Delay(float flDelay, std::function<void()> fn)
{
	Repeat(flDelay, [fn]() {
		fn();
		return false;
	});
}

///////////////////////////////////////
// Config & translations

std::vector<std::string> ReadList(KeyValues& kv, const char* szKey, const char* szDefault)
{
	return SplitString(kv.GetString(szKey, szDefault), ';');
}

void LoadConfig()
{
	g_Config = HideConfig();
	const char* pszPath = "addons/configs/admin_system/hide.ini";
	KeyValues kv("Hide");
	if (!LoadKeyValuesFile(kv, pszPath))
	{
		g_pUtils->ErrorLog("[%s] Failed to load %s, using defaults", g_PLAPI->GetLogTag(), pszPath);
		return;
	}
	g_Config.sPermission = kv.GetString("permission", "@admin/hide");
	g_Config.sAutoHidePermission = kv.GetString("auto_hide_permission", "");
	g_Config.vChatCommands = ReadList(kv, "chat_commands", "!hide;/hide");
	g_Config.vConsoleCommands = ReadList(kv, "console_commands", "mm_hide");
	g_Config.vListChatCommands = ReadList(kv, "list_chat_commands", "!hidelist");
	g_Config.vListConsoleCommands = ReadList(kv, "list_console_commands", "mm_hidelist");
	g_Config.bMenuItem = kv.GetInt("menu_item", 1) != 0;
	g_Config.sMenuCategory = kv.GetString("menu_category", "server");
	g_Config.sMenuCategoryName = kv.GetString("menu_category_name", "Category_Server");
	g_Config.bHidePawn = kv.GetInt("hide_pawn", 1) != 0;
	g_Config.bSilentTeamChange = kv.GetInt("silent_team_change", 1) != 0;
	g_Config.bSilentDeath = kv.GetInt("silent_death", 1) != 0;
	g_Config.bSilentConnect = kv.GetInt("silent_connect", 1) != 0;
	g_Config.bSilentDisconnect = kv.GetInt("silent_disconnect", 1) != 0;
	g_Config.bBlockChat = kv.GetInt("block_chat", 1) != 0;
	g_Config.bBlockVoice = kv.GetInt("block_voice", 1) != 0;
	g_Config.bRehideOnSpectator = kv.GetInt("rehide_on_spectator", 1) != 0;
	g_Config.bRehideOnTeamChange = kv.GetInt("rehide_on_team_change", 1) != 0;
	g_Config.bBlockAutoTeam = kv.GetInt("block_auto_team", 1) != 0;
	g_Config.bKeepHidden = kv.GetInt("keep_hidden", 1) != 0;
	g_Config.bAutoHideOnSpectate = kv.GetInt("auto_hide_on_spectate", 0) != 0;
	g_Config.sTeamMenuRestore = kv.GetString("teamselect_menu_restore", "0");

	g_Config.vPermissions = SplitString(g_Config.sPermission, '|');
	if (g_Config.vPermissions.empty())
	{
		g_Config.sPermission = "@admin/hide";
		g_Config.vPermissions = {g_Config.sPermission};
	}
	if (g_Config.sMenuCategory.empty()) g_Config.sMenuCategory = "server";
	if (g_Config.sMenuCategoryName.empty()) g_Config.sMenuCategoryName = g_Config.sMenuCategory;
	if (g_Config.sTeamMenuRestore != "1") g_Config.sTeamMenuRestore = "0";
}

void LoadPhrases()
{
	g_mPhrases.clear();
	const char* pszPath = "addons/translations/as_hide.phrases.txt";
	KeyValues kv("Phrases");
	if (!LoadKeyValuesFile(kv, pszPath))
	{
		g_pUtils->ErrorLog("[%s] Failed to load %s", g_PLAPI->GetLogTag(), pszPath);
		return;
	}
	const char* pszLanguage = g_pUtils->GetLanguage();
	for (KeyValues* pKey = kv.GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey())
	{
		const char* szValue = pszLanguage ? pKey->GetString(pszLanguage, "") : "";
		if (!szValue[0]) szValue = pKey->GetString("en", "");
		g_mPhrases[pKey->GetName()] = szValue;
	}
}

const char* Phrase(const char* szKey)
{
	auto it = g_mPhrases.find(szKey);
	return it != g_mPhrases.end() ? it->second.c_str() : szKey;
}

void PrintText(int iSlot, const std::string& sText)
{
	std::string sMessage = std::string(Phrase("Prefix")) + sText;
	g_pUtils->PrintToChat(iSlot, "%s", sMessage.c_str());
}

void PrintPhrase(int iSlot, const char* szKey)
{
	PrintText(iSlot, Phrase(szKey));
}

///////////////////////////////////////
// Team select menu suppression (switching to "unassigned" opens it on the client)

void RestoreTeamMenu()
{
	std::string sCommand = "sv_disable_teamselect_menu " + g_Config.sTeamMenuRestore;
	RunServerCommand(sCommand.c_str());
}

// Several admins can hide at the same moment: the cvar is restored after the last one
void LockTeamMenu()
{
	if (g_iTeamMenuLocks++ == 0) RunServerCommand("sv_disable_teamselect_menu 1");
}

void UnlockTeamMenu()
{
	if (g_iTeamMenuLocks <= 0) return;
	if (--g_iTeamMenuLocks == 0) RestoreTeamMenu();
}

///////////////////////////////////////
// Hide mode

void ResetHideState(int iSlot)
{
	g_bHidden[iSlot] = false;
	g_bApplying[iSlot] = false;
	g_tSilentUntil[iSlot] = Clock::time_point();
}

// player_connect_full (fires for every player after a map change too) / disconnect
void ResetConnectionState(int iSlot, bool bConnected)
{
	g_tConnectedAt[iSlot] = bConnected ? Clock::now() : Clock::time_point();
	g_bJoinedByPlayer[iSlot] = false;
	g_iCorrections[iSlot] = 0;
	g_tVoiceNotified[iSlot] = Clock::time_point();
	g_iRestoreToken[iSlot]++;
}

// Re-hiding is limited (3 times in 30 seconds) so it never fights another plugin forever
bool CanCorrect(int iSlot)
{
	if (g_iCorrections[iSlot] == 0 || SecondsSince(g_tFirstCorrection[iSlot]) > 30.0)
	{
		g_iCorrections[iSlot] = 0;
		g_tFirstCorrection[iSlot] = Clock::now();
	}
	return ++g_iCorrections[iSlot] <= 3;
}

bool IsHiddenOrHiding(int iSlot)
{
	return g_bHidden[iSlot] || g_bApplying[iSlot];
}

bool IsTeamChangeSilenced(int iSlot)
{
	return IsHiddenOrHiding(iSlot) || Clock::now() < g_tSilentUntil[iSlot];
}

bool HasHideAccess(int iSlot)
{
	for (const auto& sPermission : g_Config.vPermissions)
	{
		if (g_pAdmin->HasPermission(iSlot, sPermission.c_str())) return true;
	}
	return false;
}

// auto_hide_permission has to be given explicitly: @admin/root alone does not count
bool HasAutoHidePermission(int iSlot)
{
	if (g_Config.sAutoHidePermission.empty()) return false;
	for (const auto& sPermission : g_pAdmin->GetAdminPermissions(iSlot))
	{
		if (sPermission == g_Config.sAutoHidePermission) return true;
	}
	return false;
}

// Should this admin be hidden as soon as he is in game (map change, reconnect, auto hide)
bool WantsHide(int iSlot, uint64 iSteamID64)
{
	if (!iSteamID64) return false;
	if (g_Config.bKeepHidden && g_setKeepHidden.count(iSteamID64)) return true;
	return !g_setAutoHideOptOut.count(iSteamID64) && HasAutoHidePermission(iSlot);
}

// The admin wants to stay hidden: remembered for map changes / reconnects
void RememberHide(int iSlot)
{
	uint64 iSteamID64 = GetSlotSteamID(iSlot);
	if (!iSteamID64) return;
	g_setAutoHideOptOut.erase(iSteamID64);
	if (g_Config.bKeepHidden) g_setKeepHidden.insert(iSteamID64);
}

// The admin is visible now: no restore / auto hide until he hides again
void ForgetHide(int iSlot)
{
	uint64 iSteamID64 = GetSlotSteamID(iSlot);
	if (!iSteamID64) return;
	g_setKeepHidden.erase(iSteamID64);
	if (HasAutoHidePermission(iSlot)) g_setAutoHideOptOut.insert(iSteamID64);
}

void EnableHide(int iSlot, const char* szNotifyPhrase)
{
	g_bHidden[iSlot] = true;
	if (g_bApplying[iSlot]) return;
	g_bApplying[iSlot] = true;

	std::string sNotify = szNotifyPhrase ? szNotifyPhrase : "";
	LockTeamMenu();
	Delay(0.1f, [iSlot, sNotify]() {
		CEntityInstance* pController = GetController(iSlot);
		if (g_bHidden[iSlot] && pController && g_pPlayers->IsInGame(iSlot))
		{
			if (IsAlive(GetHandleField(pController, g_fPlayerPawn))) g_pPlayers->CommitSuicide(iSlot, false, true);
			// Through spectators first so the client gets a free observer camera,
			// then "unassigned": players without a team are not listed in the scoreboard
			g_pPlayers->ChangeTeam(iSlot, TEAM_SPECTATOR);
			g_pPlayers->ChangeTeam(iSlot, TEAM_NONE);
			g_pAdmin->SendAction(iSlot, "hide_on", "");
			if (!sNotify.empty()) PrintPhrase(iSlot, sNotify.c_str());
		}
		else g_bHidden[iSlot] = false;
		g_bApplying[iSlot] = false;
		Delay(0.5f, UnlockTeamMenu);
	});
}

void DisableHide(int iSlot, bool bMoveToSpectators, bool bSilent, const char* szNotifyPhrase)
{
	if (!g_bHidden[iSlot]) return;
	g_bHidden[iSlot] = false;
	ForgetHide(iSlot);
	if (bSilent) g_tSilentUntil[iSlot] = Clock::now() + std::chrono::milliseconds(500);
	g_pAdmin->SendAction(iSlot, "hide_off", "");
	if (bMoveToSpectators) g_pPlayers->ChangeTeam(iSlot, TEAM_SPECTATOR);
	if (szNotifyPhrase) PrintPhrase(iSlot, szNotifyPhrase);
}

void ToggleHide(int iSlot)
{
	if (!IsValidSlot(iSlot)) return;
	if (!HasHideAccess(iSlot))
	{
		PrintPhrase(iSlot, "NoAccess");
		return;
	}
	if (g_bApplying[iSlot]) return;
	if (g_bHidden[iSlot])
	{
		DisableHide(iSlot, true, true, "Hide_Off");
		return;
	}
	RememberHide(iSlot);
	EnableHide(iSlot, "Hide_On");
}

// Hides an admin that comes back after a map change / reconnect (keep_hidden) or has
// auto_hide_permission. Waits until the player is loaded and his admin data is in.
void StartRestore(int iSlot)
{
	uint64 iSteamID64 = GetSlotSteamID(iSlot);
	if (!iSteamID64) return;
	bool bKept = g_Config.bKeepHidden && g_setKeepHidden.count(iSteamID64);
	if (!bKept && g_Config.sAutoHidePermission.empty()) return;

	int iToken = ++g_iRestoreToken[iSlot];
	auto pAttempts = std::make_shared<int>(0);
	Repeat(1.0f, [iSlot, iSteamID64, iToken, pAttempts]() {
		if (g_iRestoreToken[iSlot] != iToken || ++(*pAttempts) > 120) return false;
		if (IsHiddenOrHiding(iSlot) || g_bJoinedByPlayer[iSlot] || GetSlotSteamID(iSlot) != iSteamID64) return false;
		if (g_tConnectedAt[iSlot] == Clock::time_point() || !g_pPlayers->IsInGame(iSlot) || !GetController(iSlot)) return true;
		// admin data is loaded asynchronously after Steam authorization
		if (!HasHideAccess(iSlot)) return true;
		if (!WantsHide(iSlot, iSteamID64)) return false;
		bool bKeptNow = g_Config.bKeepHidden && g_setKeepHidden.count(iSteamID64);
		RememberHide(iSlot);
		EnableHide(iSlot, bKeptNow ? "Hide_Restored" : "Hide_Auto");
		return false;
	});
}

// "jointeam 2 1" (Utils 1.8+) or "2 1" (older Utils)
int ParseJoinTeam(const char* szContent)
{
	for (std::string sArg : SplitString(szContent ? szContent : "", ' '))
	{
		sArg.erase(std::remove(sArg.begin(), sArg.end(), '"'), sArg.end());
		if (!sArg.empty() && sArg.find_first_not_of("0123456789") == std::string::npos) return atoi(sArg.c_str());
	}
	return -1;
}

// returning true swallows the command
bool OnJoinTeam(int iSlot, const char* szContent)
{
	if (!IsValidSlot(iSlot)) return false;
	if (g_bApplying[iSlot]) return true;

	uint64 iSteamID64 = GetSlotSteamID(iSlot);
	bool bWants = g_bHidden[iSlot] || WantsHide(iSlot, iSteamID64);
	// Right after connecting / a map change the client re-joins its old team by itself:
	// keep admins that stay hidden out of it, the rest is not the player's own choice either
	if (g_tConnectedAt[iSlot] != Clock::time_point() && SecondsSince(g_tConnectedAt[iSlot]) < AUTO_JOIN_SECONDS) return bWants;

	if (!g_bHidden[iSlot] && ParseJoinTeam(szContent) == TEAM_SPECTATOR && g_Config.bAutoHideOnSpectate && HasHideAccess(iSlot))
	{
		RememberHide(iSlot);
		EnableHide(iSlot, "Hide_On");
		return true;
	}

	// The player's own choice in the team menu
	g_bJoinedByPlayer[iSlot] = true;
	if (g_bHidden[iSlot]) DisableHide(iSlot, false, false, "Hide_Off");
	else if (bWants) ForgetHide(iSlot);
	return false;
}

// mp_force_pick_time moves players without a team into one: push the deadline away for hidden admins
bool BlockAutoTeam()
{
	if (!g_Config.bBlockAutoTeam || !g_pEntitySystem) return true;
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		if (!g_bHidden[i] || g_bApplying[i]) continue;
		CEntityInstance* pController = GetController(i);
		float flForceTeamTime;
		if (!ReadField(pController, g_fForceTeamTime, flForceTeamTime)) continue;
		if (flForceTeamTime < 1.0e8f) WriteField(pController, g_fForceTeamTime, 1.0e9f);
	}
	return true;
}

void PrintHiddenList(int iSlot)
{
	std::string sNames;
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		if (!IsHiddenOrHiding(i)) continue;
		if (!sNames.empty()) sNames += ", ";
		sNames += g_pPlayers->GetPlayerName(i);
	}
	if (!IsValidSlot(iSlot))
	{
		META_CONPRINTF("[%s] %s\n", g_PLAPI->GetLogTag(), sNames.empty() ? "-" : sNames.c_str());
		return;
	}
	if (!HasHideAccess(iSlot))
	{
		PrintPhrase(iSlot, "NoAccess");
		return;
	}
	if (sNames.empty()) PrintPhrase(iSlot, "List_Empty");
	else PrintText(iSlot, std::string(Phrase("List")) + sNames);
}

///////////////////////////////////////
// Network: hidden admin's pawns are not sent to other players, so nobody
// (spectator lists, scoreboard mods, cheats) can see whom the admin is watching

void UpdateTransmitState()
{
	// entity index -> owner slot; reused every tick without allocations
	static std::vector<std::pair<int, int>> s_vecDesired;
	s_vecDesired.clear();
	if (g_Config.bHidePawn && g_pEntitySystem)
	{
		for (int i = 0; i < MAX_SLOTS; i++)
		{
			if (!g_bHidden[i] || g_bApplying[i]) continue;
			CEntityInstance* pController = GetController(i);
			if (!pController) continue;
			CEntityInstance* pObserverPawn = GetHandleField(pController, g_fObserverPawn);
			if (pObserverPawn) s_vecDesired.emplace_back(GetEntityIndex(pObserverPawn), i);
			// dead body (never hide a living pawn, it would desync the game for others)
			CEntityInstance* pPawn = GetHandleField(pController, g_fPlayerPawn);
			if (pPawn && !IsAlive(pPawn)) s_vecDesired.emplace_back(GetEntityIndex(pPawn), i);
		}
	}

	auto FindDesired = [](int iEntityIndex) {
		for (const auto& entry : s_vecDesired)
		{
			if (entry.first == iEntityIndex) return entry.second;
		}
		return -1;
	};

	for (auto it = g_mTransmitHidden.begin(); it != g_mTransmitHidden.end();)
	{
		if (FindDesired(it->first) != it->second)
		{
			g_pUtils->SetTransmitState(it->first, true, {});
			it = g_mTransmitHidden.erase(it);
		}
		else ++it;
	}

	for (const auto& [iEntityIndex, iOwner] : s_vecDesired)
	{
		if (g_mTransmitHidden.count(iEntityIndex)) continue;
		std::vector<int> vecSlots;
		vecSlots.reserve(MAX_SLOTS - 1);
		for (int i = 0; i < MAX_SLOTS; i++)
		{
			// the admin must keep receiving his own pawns
			if (i != iOwner) vecSlots.push_back(i);
		}
		g_pUtils->SetTransmitState(iEntityIndex, false, vecSlots);
		g_mTransmitHidden[iEntityIndex] = iOwner;
	}
}

void HideEntityListener::CheckTransmit(CCheckTransmitInfo **pInfoInfoList, int nInfoCount, CBitVec<16384> &unionTransmitEdicts, CBitVec<16384> &, const Entity2Networkable_t **pNetworkables, const uint16 *pEntityIndicies, int nEntityIndices, bool bEnablePVSBits)
{
	// Called by Utils right before it applies SetTransmitState for this tick
	UpdateTransmitState();
}

///////////////////////////////////////
// Admin menu

void RegisterMenuItem()
{
	if (!g_Config.bMenuItem || g_bMenuRegistered) return;
	g_szMenuIdentity = strdup("hide");
	g_szMenuName = strdup("Hide_Item");
	g_szMenuCategory = strdup(g_Config.sMenuCategory.c_str());
	g_szMenuCategoryName = strdup(g_Config.sMenuCategoryName.c_str());
	g_szMenuFlags = strdup(g_Config.sPermission.c_str());

	g_pAdmin->RegisterCategory(g_szMenuCategory, g_szMenuCategoryName, nullptr);
	g_pAdmin->RegisterItem(g_szMenuIdentity, g_szMenuName, g_szMenuCategory, g_szMenuFlags,
		[](int iSlot, const char* szCategory, const char* szIdentity, std::string& szItem) {
			bool bHidden = IsValidSlot(iSlot) && IsHiddenOrHiding(iSlot);
			szItem = std::string(Phrase("Menu_Item")) + " " + Phrase(bHidden ? "Menu_State_On" : "Menu_State_Off");
		},
		[](int iSlot, const char* szCategory, const char* szIdentity, const char* szItem) {
			ToggleHide(iSlot);
			g_pAdmin->ShowAdminLastCategoryMenu(iSlot);
		});
	g_bMenuRegistered = true;
}

///////////////////////////////////////
// Events

int GetEventSlot(IGameEvent* pEvent)
{
	int iSlot = pEvent->GetPlayerSlot("userid").Get();
	return IsValidSlot(iSlot) ? iSlot : -1;
}

// After a team change of a hidden admin. His own jointeam turns hide mode off before it,
// so here it is the server (mp_force_pick_time), a balancer, another plugin or admin
void OnPlayerTeam(IGameEvent* pEvent)
{
	int iSlot = GetEventSlot(pEvent);
	if (iSlot == -1 || !g_bHidden[iSlot] || g_bApplying[iSlot] || pEvent->GetBool("disconnect")) return;
	int iTeam = pEvent->GetInt("team");
	if (iTeam == TEAM_NONE) return;
	bool bRehide = (iTeam == TEAM_SPECTATOR && g_Config.bRehideOnSpectator) ||
		((iTeam == TEAM_T || iTeam == TEAM_CT) && g_Config.bRehideOnTeamChange);
	if (bRehide && CanCorrect(iSlot))
	{
		Delay(0.1f, [iSlot, iTeam]() {
			if (!g_bHidden[iSlot] || g_bApplying[iSlot]) return;
			if (GetTeam(GetController(iSlot)) != TEAM_NONE) EnableHide(iSlot, iTeam == TEAM_SPECTATOR ? nullptr : "Hide_Rehidden");
		});
	}
	else DisableHide(iSlot, false, false, "Hide_Off_Team");
}

void OnPlayerConnectFull(IGameEvent* pEvent)
{
	int iSlot = GetEventSlot(pEvent);
	if (iSlot == -1) return;
	// a new connection or map: the hide state of the previous one is gone
	ResetHideState(iSlot);
	ResetConnectionState(iSlot, true);
	StartRestore(iSlot);
}

void OnPlayerDisconnect(IGameEvent* pEvent)
{
	int iSlot = GetEventSlot(pEvent);
	if (iSlot == -1) return;
	// g_setKeepHidden keeps the SteamID: hide mode comes back after reconnect
	ResetHideState(iSlot);
	ResetConnectionState(iSlot, false);
}

void HookEvents()
{
	g_pUtils->HookEvent(g_PLID, "player_connect_full", [](const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
		OnPlayerConnectFull(pEvent);
	});

	if (!g_bExtendedApi)
	{
		// Older Utils: HookEvent runs before the engine handles the event
		g_pUtils->HookEvent(g_PLID, "player_team", [](const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
			OnPlayerTeam(pEvent);
		});
		g_pUtils->HookEvent(g_PLID, "player_disconnect", [](const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
			OnPlayerDisconnect(pEvent);
		});
		return;
	}

	// Post hooks run after the pre hooks below, so those still see the admin as hidden
	g_pUtils->HookEventPost(g_PLID, "player_team", [](const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
		OnPlayerTeam(pEvent);
	});
	g_pUtils->HookEventPost(g_PLID, "player_disconnect", [](const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
		OnPlayerDisconnect(pEvent);
	});

	// "X is joining the Spectators" / unassigned messages of a hidden admin
	g_pUtils->HookEventPre(g_PLID, "player_team", [](const char* szName, IGameEvent* pEvent, EventInfo* pInfo) {
		if (!g_Config.bSilentTeamChange) return EventHookResult::Continue;
		int iSlot = GetEventSlot(pEvent);
		if (iSlot == -1 || !IsTeamChangeSilenced(iSlot)) return EventHookResult::Continue;
		pEvent->SetBool("silent", true);
		pInfo->bDontBroadcast = true;
		return EventHookResult::Changed;
	});
	// kill feed entry of the suicide that puts an alive admin into hide mode
	g_pUtils->HookEventPre(g_PLID, "player_death", [](const char* szName, IGameEvent* pEvent, EventInfo* pInfo) {
		if (!g_Config.bSilentDeath) return EventHookResult::Continue;
		int iSlot = GetEventSlot(pEvent);
		if (iSlot == -1 || !IsHiddenOrHiding(iSlot)) return EventHookResult::Continue;
		pInfo->bDontBroadcast = true;
		return EventHookResult::Changed;
	});
	// "X connected": only known for admins that come back hidden (keep_hidden)
	g_pUtils->HookEventPre(g_PLID, "player_connect", [](const char* szName, IGameEvent* pEvent, EventInfo* pInfo) {
		if (!g_Config.bSilentConnect || !g_Config.bKeepHidden || pEvent->GetBool("bot")) return EventHookResult::Continue;
		uint64 iSteamID64 = pEvent->GetUint64("xuid");
		if (!iSteamID64 || !g_setKeepHidden.count(iSteamID64)) return EventHookResult::Continue;
		pInfo->bDontBroadcast = true;
		return EventHookResult::Changed;
	});
	g_pUtils->HookEventPre(g_PLID, "player_disconnect", [](const char* szName, IGameEvent* pEvent, EventInfo* pInfo) {
		if (!g_Config.bSilentDisconnect) return EventHookResult::Continue;
		int iSlot = GetEventSlot(pEvent);
		if (iSlot == -1 || !IsHiddenOrHiding(iSlot)) return EventHookResult::Continue;
		pInfo->bDontBroadcast = true;
		return EventHookResult::Changed;
	});
}

///////////////////////////////////////
// Accidental reveal: chat and voice of a hidden admin

bool OnChatPre(int iSlot, const char* szContent, bool bTeam)
{
	if (!g_Config.bBlockChat || !IsValidSlot(iSlot) || !IsHiddenOrHiding(iSlot)) return true;
	const char* szText = szContent ? szContent : "";
	while (*szText == ' ' || *szText == '"') szText++;
	// commands go to their plugins, @ in team chat goes to the admin chat module:
	// those plugins still get the message, it is only not shown to the players
	bool bCommand = *szText == '!' || *szText == '/' || (bTeam && *szText == '@');
	if (*szText && !bCommand) PrintPhrase(iSlot, "Chat_Blocked");
	return false;
}

bool OnHearingClient(int iSlot)
{
	if (!g_Config.bBlockVoice || !IsValidSlot(iSlot) || !g_bHidden[iSlot]) return true;
	if (SecondsSince(g_tVoiceNotified[iSlot]) > 10.0)
	{
		g_tVoiceNotified[iSlot] = Clock::now();
		PrintPhrase(iSlot, "Voice_Blocked");
	}
	return false;
}

///////////////////////////////////////

bool Hide::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	g_SMAPI->AddListener( this, this );

	for (int i = 0; i < MAX_SLOTS; i++)
	{
		ResetHideState(i);
		ResetConnectionState(i, false);
	}

	return true;
}

bool Hide::Unload(char *error, size_t maxlen)
{
	int ret;
	g_SMAPI->MetaFactory(UTILS_INTERFACE, &ret, NULL);
	if (g_pUtils && ret != META_IFACE_FAILED)
	{
		for (CTimer* pTimer : g_setTimers) g_pUtils->RemoveTimer(pTimer);
		if (g_bExtendedApi)
		{
			for (const auto& [iEntityIndex, iOwner] : g_mTransmitHidden) g_pUtils->SetTransmitState(iEntityIndex, true, {});
		}
		// commands, events and listeners are removed by Utils itself (OnPluginUnload)
	}
	g_setTimers.clear();
	g_mTransmitHidden.clear();

	if (g_iTeamMenuLocks > 0) RestoreTeamMenu();
	g_iTeamMenuLocks = 0;

	// The core has no way to remove a menu item: keep it, but without callbacks into this (unloaded) module
	g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, NULL);
	if (g_bMenuRegistered && g_pAdmin && ret != META_IFACE_FAILED)
	{
		g_pAdmin->RegisterItem(g_szMenuIdentity, g_szMenuName, g_szMenuCategory, g_szMenuFlags, nullptr, nullptr);
	}
	g_bMenuRegistered = false;

	return true;
}

void* Hide::OnMetamodQuery(const char* iface, int* ret)
{
	if (iface && !strcmp(iface, HIDE_INTERFACE))
	{
		if (ret) *ret = META_IFACE_OK;
		return &g_HideApi;
	}
	if (ret) *ret = META_IFACE_FAILED;
	return nullptr;
}

void Hide::AllPluginsLoaded()
{
	char error[64];
	int ret;
	g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(UTILS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		RunServerCommand(sBuffer.c_str());
		return;
	}
	g_pPlayers = (IPlayersApi *)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_pUtils->ErrorLog("[%s] Missing Players system plugin", GetLogTag());

		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		RunServerCommand(sBuffer.c_str());
		return;
	}
	g_pAdmin = (IAdminApi *)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_pUtils->ErrorLog("[%s] Missing Admin system plugin", GetLogTag());

		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		RunServerCommand(sBuffer.c_str());
		return;
	}

	g_SMAPI->MetaFactory(LAYOUT_INTERFACE_NAME, &ret, NULL);
	g_bExtendedApi = ret != META_IFACE_FAILED;
	if (g_bExtendedApi) META_CONPRINTF("[%s] Utils %s: full mode\n", GetLogTag(), g_pUtils->GetVersion());
	else g_pUtils->ErrorLog("[%s] Utils is older than 1.9.1: only the scoreboard is hidden (no spectator lists, no silent events). Update Utils.", GetLogTag());

	LoadConfig();
	LoadPhrases();

	// late load: the server is already running
	StartupServer();
	g_pUtils->StartupServer(g_PLID, StartupServer);

	// admin data is loaded after authorization: try the restore again (it waits for it)
	g_pPlayers->HookOnClientAuthorized(g_PLID, [](int iSlot, uint64 iSteamID64) {
		if (IsValidSlot(iSlot) && !IsHiddenOrHiding(iSlot)) StartRestore(iSlot);
	});

	g_pUtils->RegCommand(g_PLID, g_Config.vConsoleCommands, g_Config.vChatCommands, [](int iSlot, const char* szContent) {
		if (!IsValidSlot(iSlot))
		{
			META_CONPRINTF("[%s] This command is only available to players\n", g_PLAPI->GetLogTag());
			return true;
		}
		ToggleHide(iSlot);
		return true;
	});
	g_pUtils->RegCommand(g_PLID, g_Config.vListConsoleCommands, g_Config.vListChatCommands, [](int iSlot, const char* szContent) {
		PrintHiddenList(iSlot);
		return true;
	});
	g_pUtils->RegCommand(g_PLID, {"jointeam"}, {}, OnJoinTeam);
	g_pUtils->AddChatListenerPre(g_PLID, OnChatPre);
	g_pUtils->HookIsHearingClient(g_PLID, OnHearingClient);

	HookEvents();
	Repeat(1.0f, BlockAutoTeam);
	if (g_bExtendedApi) g_pUtils->AddEntityListener(g_PLID, &g_EntityListener);

	if (g_pAdmin->IsCoreLoaded()) RegisterMenuItem();
	else
	{
		Repeat(1.0f, []() {
			if (!g_pAdmin->IsCoreLoaded()) return true;
			RegisterMenuItem();
			return false;
		});
	}
}

///////////////////////////////////////
const char* Hide::GetLicense()
{
	return "GPL";
}

const char* Hide::GetVersion()
{
	return "2.2.0";
}

const char* Hide::GetDate()
{
	return __DATE__;
}

const char *Hide::GetLogTag()
{
	return "Hide";
}

const char* Hide::GetAuthor()
{
	return "glazki";
}

const char* Hide::GetDescription()
{
	return "AS Hide: hides an admin from the scoreboard and spectator lists";
}

const char* Hide::GetName()
{
	return "[AS] Hide";
}

const char* Hide::GetURL()
{
	return "https://github.com/glazki2/cs2spy";
}
