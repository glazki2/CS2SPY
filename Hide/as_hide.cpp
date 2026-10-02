#include <stdio.h>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
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

struct HideConfig
{
	std::string sPermission = "@admin/hide";
	std::vector<std::string> vChatCommands = {"!hide", "/hide"};
	std::vector<std::string> vConsoleCommands = {"mm_hide"};
	bool bMenuItem = true;
	std::string sMenuCategory = "server";
	std::string sMenuCategoryName = "Category_Server";
	bool bHidePawn = true;
	bool bSilentTeamChange = true;
	bool bSilentDeath = true;
	bool bSilentDisconnect = true;
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
std::chrono::steady_clock::time_point g_tSilentUntil[MAX_SLOTS];
// player_connect_full: the client joins its old team by itself right after a map change
std::chrono::steady_clock::time_point g_tConnectedAt[MAX_SLOTS];
// The player picked a team himself (jointeam) since connecting
bool g_bJoinedByPlayer[MAX_SLOTS];
// Hidden admin put into a team by the server / other plugins: limited number of re-hides
int g_iCorrections[MAX_SLOTS];
std::chrono::steady_clock::time_point g_tFirstCorrection[MAX_SLOTS];
// SteamID64 of admins that should get hide mode back after a map change / reconnect
std::unordered_set<uint64> g_setKeepHidden;

// Utils 1.9.1+: SetTransmitState, entity listeners and pre event hooks are available
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

int GetSchemaOffset(const char* szClass, const char* szField)
{
	static std::map<std::string, int> s_mOffsets;
	std::string sKey = std::string(szClass) + "::" + szField;
	auto it = s_mOffsets.find(sKey);
	if (it != s_mOffsets.end()) return it->second;

	CSchemaSystemTypeScope* pScope = g_pSchemaSystem->FindTypeScopeForModule(SERVER_MODULE);
	CSchemaClassInfo* pClass = pScope ? pScope->FindDeclaredClass(szClass).Get() : nullptr;
	int iOffset = pClass ? FindSchemaOffset(pClass, szField) : -1;
	// logged and cached once: this is called every tick
	if (iOffset < 0) g_pUtils->ErrorLog("[%s] Schema field %s not found", g_PLAPI->GetLogTag(), sKey.c_str());
	s_mOffsets[sKey] = iOffset;
	return iOffset;
}

template<typename T>
bool ReadField(CEntityInstance* pEntity, const char* szClass, const char* szField, T& value)
{
	if (!pEntity) return false;
	int iOffset = GetSchemaOffset(szClass, szField);
	if (iOffset < 0) return false;
	value = *reinterpret_cast<T*>(reinterpret_cast<uint8*>(pEntity) + iOffset);
	return true;
}

template<typename T>
bool WriteField(CEntityInstance* pEntity, const char* szClass, const char* szField, const T& value)
{
	if (!pEntity) return false;
	int iOffset = GetSchemaOffset(szClass, szField);
	if (iOffset < 0) return false;
	*reinterpret_cast<T*>(reinterpret_cast<uint8*>(pEntity) + iOffset) = value;
	g_pUtils->SetStateChanged(reinterpret_cast<CBaseEntity*>(pEntity), szClass, szField);
	return true;
}

CEntityInstance* GetController(int iSlot)
{
	if (!g_pEntitySystem) return nullptr;
	CEntityInstance* pController = g_pEntitySystem->GetEntityInstance(CEntityIndex(iSlot + 1));
	if (!pController || strcmp(pController->GetClassname(), "cs_player_controller")) return nullptr;
	return pController;
}

CEntityInstance* GetHandleField(CEntityInstance* pEntity, const char* szClass, const char* szField)
{
	CEntityHandle hEntity;
	if (!ReadField(pEntity, szClass, szField, hEntity) || !hEntity.IsValid()) return nullptr;
	return g_pEntitySystem->GetEntityInstance(hEntity);
}

// m_hPlayerPawn - the player model (CCSPlayerPawn)
CEntityInstance* GetPlayerPawn(CEntityInstance* pController)
{
	return GetHandleField(pController, "CCSPlayerController", "m_hPlayerPawn");
}

// m_hObserverPawn - free camera / spectating pawn (CCSObserverPawn), holds the observer target
CEntityInstance* GetObserverPawn(CEntityInstance* pController)
{
	return GetHandleField(pController, "CCSPlayerController", "m_hObserverPawn");
}

// m_hPawn - the pawn the player currently controls (player or observer pawn)
CEntityInstance* GetCurrentPawn(CEntityInstance* pController)
{
	return GetHandleField(pController, "CBasePlayerController", "m_hPawn");
}

int GetTeam(CEntityInstance* pEntity)
{
	uint8 iTeam;
	return ReadField(pEntity, "CBaseEntity", "m_iTeamNum", iTeam) ? iTeam : -1;
}

bool IsAlive(CEntityInstance* pEntity)
{
	uint8 iLifeState;
	return ReadField(pEntity, "CBaseEntity", "m_lifeState", iLifeState) && iLifeState == LIFE_ALIVE;
}

bool IsValidSlot(int iSlot)
{
	return iSlot >= 0 && iSlot < MAX_SLOTS;
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

bool IsUtilsVersionAtLeast(int iMajor, int iMinor, int iPatch)
{
	const char* szVersion = g_pUtils->GetVersion();
	int a = 0, b = 0, c = 0;
	if (!szVersion || sscanf(szVersion, "%d.%d.%d", &a, &b, &c) < 2) return false;
	return std::tie(a, b, c) >= std::tie(iMajor, iMinor, iPatch);
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

void LoadConfig()
{
	g_Config = HideConfig();
	const char* pszPath = "addons/configs/admin_system/hide.ini";
	KeyValues kv("Hide");
	if (!kv.LoadFromFile(g_pFullFileSystem, pszPath))
	{
		g_pUtils->ErrorLog("[%s] Failed to load %s, using defaults", g_PLAPI->GetLogTag(), pszPath);
		return;
	}
	g_Config.sPermission = kv.GetString("permission", "@admin/hide");
	g_Config.vChatCommands = SplitString(kv.GetString("chat_commands", "!hide;/hide"), ';');
	g_Config.vConsoleCommands = SplitString(kv.GetString("console_commands", "mm_hide"), ';');
	g_Config.bMenuItem = kv.GetInt("menu_item", 1) != 0;
	g_Config.sMenuCategory = kv.GetString("menu_category", "server");
	g_Config.sMenuCategoryName = kv.GetString("menu_category_name", "Category_Server");
	g_Config.bHidePawn = kv.GetInt("hide_pawn", 1) != 0;
	g_Config.bSilentTeamChange = kv.GetInt("silent_team_change", 1) != 0;
	g_Config.bSilentDeath = kv.GetInt("silent_death", 1) != 0;
	g_Config.bSilentDisconnect = kv.GetInt("silent_disconnect", 1) != 0;
	g_Config.bRehideOnSpectator = kv.GetInt("rehide_on_spectator", 1) != 0;
	g_Config.bRehideOnTeamChange = kv.GetInt("rehide_on_team_change", 1) != 0;
	g_Config.bBlockAutoTeam = kv.GetInt("block_auto_team", 1) != 0;
	g_Config.bKeepHidden = kv.GetInt("keep_hidden", 1) != 0;
	g_Config.bAutoHideOnSpectate = kv.GetInt("auto_hide_on_spectate", 0) != 0;
	g_Config.sTeamMenuRestore = kv.GetString("teamselect_menu_restore", "0");

	if (g_Config.sMenuCategory.empty()) g_Config.sMenuCategory = "server";
	if (g_Config.sMenuCategoryName.empty()) g_Config.sMenuCategoryName = g_Config.sMenuCategory;
}

void LoadPhrases()
{
	g_mPhrases.clear();
	const char* pszPath = "addons/translations/as_hide.phrases.txt";
	KeyValues kv("Phrases");
	if (!kv.LoadFromFile(g_pFullFileSystem, pszPath))
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

void PrintPhrase(int iSlot, const char* szKey)
{
	std::string sText = std::string(Phrase("Prefix")) + Phrase(szKey);
	g_pUtils->PrintToChat(iSlot, "%s", sText.c_str());
}

///////////////////////////////////////
// Team select menu suppression (switching to "unassigned" opens it on the client)

void RestoreTeamMenu()
{
	std::string sCommand = "sv_disable_teamselect_menu " + g_Config.sTeamMenuRestore;
	engine->ServerCommand(sCommand.c_str());
}

// Several admins can hide at the same moment: the cvar is restored after the last one
void LockTeamMenu()
{
	if (g_iTeamMenuLocks++ == 0) engine->ServerCommand("sv_disable_teamselect_menu 1");
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
	g_tSilentUntil[iSlot] = std::chrono::steady_clock::time_point();
}

// player_connect_full (also fires for every player after a map change) / disconnect
void ResetConnectionState(int iSlot, bool bConnected)
{
	g_tConnectedAt[iSlot] = bConnected ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
	g_bJoinedByPlayer[iSlot] = false;
	g_iCorrections[iSlot] = 0;
}

double SecondsSince(std::chrono::steady_clock::time_point tPoint)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - tPoint).count();
}

// Re-hiding is limited (3 times in 30 seconds) so it never fights another plugin forever
bool CanCorrect(int iSlot)
{
	if (g_iCorrections[iSlot] == 0 || SecondsSince(g_tFirstCorrection[iSlot]) > 30.0)
	{
		g_iCorrections[iSlot] = 0;
		g_tFirstCorrection[iSlot] = std::chrono::steady_clock::now();
	}
	return ++g_iCorrections[iSlot] <= 3;
}

bool IsTeamChangeSilenced(int iSlot)
{
	return g_bHidden[iSlot] || g_bApplying[iSlot] || std::chrono::steady_clock::now() < g_tSilentUntil[iSlot];
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
			if (IsAlive(GetPlayerPawn(pController))) g_pPlayers->CommitSuicide(iSlot, false, true);
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
	g_setKeepHidden.erase(g_pPlayers->GetSteamID64(iSlot));
	if (bSilent) g_tSilentUntil[iSlot] = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
	g_pAdmin->SendAction(iSlot, "hide_off", "");
	if (bMoveToSpectators) g_pPlayers->ChangeTeam(iSlot, TEAM_SPECTATOR);
	if (szNotifyPhrase) PrintPhrase(iSlot, szNotifyPhrase);
}

bool HasHideAccess(int iSlot)
{
	return g_pAdmin->HasPermission(iSlot, g_Config.sPermission.c_str());
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
	if (g_Config.bKeepHidden) g_setKeepHidden.insert(g_pPlayers->GetSteamID64(iSlot));
	EnableHide(iSlot, "Hide_On");
}

// Gives hide mode back to an admin that had it before a map change / reconnect
void RestoreHide(int iSlot, uint64 iSteamID64)
{
	auto pAttempts = std::make_shared<int>(0);
	Repeat(1.0f, [iSlot, iSteamID64, pAttempts]() {
		if (++(*pAttempts) > 60) return false;
		if (g_pPlayers->GetSteamID64(iSlot) != iSteamID64) return false;
		if (!g_setKeepHidden.count(iSteamID64) || g_bHidden[iSlot]) return false;
		// admin data is loaded asynchronously and the player has to be fully in game
		if (!g_pPlayers->IsInGame(iSlot) || !HasHideAccess(iSlot)) return true;
		CEntityInstance* pController = GetController(iSlot);
		if (!pController || !GetCurrentPawn(pController)) return true;
		int iTeam = GetTeam(pController);
		if ((iTeam == TEAM_T || iTeam == TEAM_CT) && g_bJoinedByPlayer[iSlot])
		{
			// picked a team in the menu himself
			g_setKeepHidden.erase(iSteamID64);
			return false;
		}
		EnableHide(iSlot, "Hide_Restored");
		return false;
	});
}

///////////////////////////////////////
// Network: hidden admin's pawns are not sent to other players, so nobody
// (spectator lists, scoreboard mods, cheats) can see whom the admin is watching

void UpdateTransmitState()
{
	std::map<int, int> mDesired;
	if (g_Config.bHidePawn && g_pEntitySystem)
	{
		for (int i = 0; i < MAX_SLOTS; i++)
		{
			if (!g_bHidden[i] || g_bApplying[i]) continue;
			CEntityInstance* pController = GetController(i);
			if (!pController) continue;
			CEntityInstance* pObserverPawn = GetObserverPawn(pController);
			if (pObserverPawn) mDesired[pObserverPawn->GetEntityIndex().Get()] = i;
			// dead body (never hide a living pawn, it would desync the game for others)
			CEntityInstance* pPawn = GetPlayerPawn(pController);
			if (pPawn && !IsAlive(pPawn)) mDesired[pPawn->GetEntityIndex().Get()] = i;
		}
	}

	for (auto it = g_mTransmitHidden.begin(); it != g_mTransmitHidden.end();)
	{
		auto itDesired = mDesired.find(it->first);
		if (itDesired == mDesired.end() || itDesired->second != it->second)
		{
			g_pUtils->SetTransmitState(it->first, true, {});
			it = g_mTransmitHidden.erase(it);
		}
		else ++it;
	}

	for (const auto& [iEntityIndex, iOwner] : mDesired)
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
			bool bHidden = IsValidSlot(iSlot) && g_bHidden[iSlot];
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

void HookEventsExtended()
{
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
		if (iSlot == -1 || (!g_bApplying[iSlot] && !g_bHidden[iSlot])) return EventHookResult::Continue;
		pInfo->bDontBroadcast = true;
		return EventHookResult::Changed;
	});
	g_pUtils->HookEventPre(g_PLID, "player_disconnect", [](const char* szName, IGameEvent* pEvent, EventInfo* pInfo) {
		if (!g_Config.bSilentDisconnect) return EventHookResult::Continue;
		int iSlot = GetEventSlot(pEvent);
		if (iSlot == -1 || !g_bHidden[iSlot]) return EventHookResult::Continue;
		pInfo->bDontBroadcast = true;
		return EventHookResult::Changed;
	});
}

void HookEvents()
{
	g_pUtils->HookEvent(g_PLID, "player_team", [](const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
		int iSlot = GetEventSlot(pEvent);
		if (iSlot == -1 || !g_bHidden[iSlot] || g_bApplying[iSlot]) return;
		if (pEvent->GetBool("disconnect")) return;
		int iTeam = pEvent->GetInt("team");
		// The admin's own jointeam turns hide mode off before this event,
		// so here it is the server (mp_force_pick_time), a balancer, another plugin or admin
		bool bRehide = (iTeam == TEAM_SPECTATOR && g_Config.bRehideOnSpectator) ||
			((iTeam == TEAM_T || iTeam == TEAM_CT) && g_Config.bRehideOnTeamChange);
		if (iTeam == TEAM_NONE) return;
		if (bRehide && CanCorrect(iSlot))
		{
			Delay(0.1f, [iSlot, iTeam]() {
				if (!g_bHidden[iSlot] || g_bApplying[iSlot]) return;
				if (GetTeam(GetController(iSlot)) != TEAM_NONE) EnableHide(iSlot, iTeam == TEAM_SPECTATOR ? nullptr : "Hide_Rehidden");
			});
		}
		else DisableHide(iSlot, false, false, "Hide_Off_Team");
	});
	g_pUtils->HookEvent(g_PLID, "player_connect_full", [](const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
		int iSlot = GetEventSlot(pEvent);
		if (iSlot != -1) ResetConnectionState(iSlot, true);
	});
	g_pUtils->HookEvent(g_PLID, "player_disconnect", [](const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
		int iSlot = GetEventSlot(pEvent);
		// g_setKeepHidden keeps the SteamID, hide mode comes back after reconnect
		if (iSlot == -1) return;
		ResetHideState(iSlot);
		ResetConnectionState(iSlot, false);
	});
}

///////////////////////////////////////

bool OnJoinTeam(int iSlot, const char* szContent)
{
	if (!IsValidSlot(iSlot)) return false;
	// szContent is "jointeam <team> ..."
	int iTeam = -1;
	sscanf(szContent, "%*s %d", &iTeam);

	if (g_bApplying[iSlot]) return true;

	uint64 iSteamID64 = g_pPlayers->GetSteamID64(iSlot);
	bool bPendingRestore = !g_bHidden[iSlot] && g_Config.bKeepHidden && g_setKeepHidden.count(iSteamID64);
	// Right after a map change the client re-joins its old team by itself: keep the admin unassigned
	if (bPendingRestore && SecondsSince(g_tConnectedAt[iSlot]) < 3.0) return true;

	if (!g_bHidden[iSlot] && iTeam == TEAM_SPECTATOR && g_Config.bAutoHideOnSpectate && HasHideAccess(iSlot))
	{
		if (g_Config.bKeepHidden) g_setKeepHidden.insert(iSteamID64);
		EnableHide(iSlot, "Hide_On");
		return true;
	}

	// The admin's own choice in the team menu
	g_bJoinedByPlayer[iSlot] = true;
	if (g_bHidden[iSlot]) DisableHide(iSlot, false, false, "Hide_Off");
	else if (bPendingRestore) g_setKeepHidden.erase(iSteamID64);
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
		if (!ReadField(pController, "CCSPlayerController", "m_flForceTeamTime", flForceTeamTime)) continue;
		if (flForceTeamTime < 1.0e8f) WriteField(pController, "CCSPlayerController", "m_flForceTeamTime", 1.0e9f);
	}
	return true;
}

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

	if (g_iTeamMenuLocks > 0 && engine) RestoreTeamMenu();
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
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pPlayers = (IPlayersApi *)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_pUtils->ErrorLog("[%s] Missing Players system plugin", GetLogTag());

		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pAdmin = (IAdminApi *)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_pUtils->ErrorLog("[%s] Missing Admin system plugin", GetLogTag());

		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}

	g_bExtendedApi = IsUtilsVersionAtLeast(1, 9, 1);
	if (!g_bExtendedApi)
	{
		g_pUtils->ErrorLog("[%s] Utils %s is too old: hiding from spectator lists and silent events need Utils 1.9.1+. Only the scoreboard is hidden.", GetLogTag(), g_pUtils->GetVersion());
	}

	LoadConfig();
	LoadPhrases();

	// late load: the server is already running
	StartupServer();
	g_pUtils->StartupServer(g_PLID, StartupServer);

	g_pPlayers->HookOnClientAuthorized(g_PLID, [](int iSlot, uint64 iSteamID64) {
		if (!IsValidSlot(iSlot)) return;
		ResetHideState(iSlot);
		if (g_Config.bKeepHidden && g_setKeepHidden.count(iSteamID64)) RestoreHide(iSlot, iSteamID64);
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
	// returning true swallows the command
	g_pUtils->RegCommand(g_PLID, {"jointeam"}, {}, OnJoinTeam);

	HookEvents();
	Repeat(1.0f, BlockAutoTeam);
	if (g_bExtendedApi)
	{
		HookEventsExtended();
		g_pUtils->AddEntityListener(g_PLID, &g_EntityListener);
	}

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
	return "2.1.1";
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
	return "Pisex, glazki2";
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
