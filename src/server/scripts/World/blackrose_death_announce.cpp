/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * Blackrose: realm-wide death announcement and optional gravestone gameobject.
 * Killer text from creature or PvP hooks when available; else "Unknown".
 */

#include "Chat.h"
#include "Config.h"
#include "GameObject.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldConfig.h"
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
std::unordered_map<uint32, std::string> g_pendingKillerByPlayerLow;
std::unordered_map<uint32, std::string> g_lastWordsByPlayerLow;

std::string BrDeath_SanitizeForPipeLog(std::string value)
{
    for (char& ch : value)
        if (ch == '|')
            ch = '/';
    return value;
}

bool BrDeath_IsNoiseLastWords(std::string_view msg)
{
    if (msg.empty())
        return true;

    // Trim leading whitespace for prefix checks
    std::size_t start = 0;
    while (start < msg.size() && std::isspace(static_cast<unsigned char>(msg[start])))
        ++start;
    if (start >= msg.size())
        return true;

    std::string_view trimmed = msg.substr(start);

    auto startsWithNoCase = [](std::string_view hay, std::string_view needle) -> bool {
        if (hay.size() < needle.size())
            return false;
        for (std::size_t i = 0; i < needle.size(); ++i)
        {
            unsigned char a = static_cast<unsigned char>(hay[i]);
            unsigned char b = static_cast<unsigned char>(needle[i]);
            if (std::tolower(a) != std::tolower(b))
                return false;
        }
        return true;
    };

    if (startsWithNoCase(trimmed, "/"))
        return true;
    if (startsWithNoCase(trimmed, "."))
        return true;
    if (startsWithNoCase(trimmed, "syntax"))
        return true;
    if (startsWithNoCase(trimmed, "unknown command"))
        return true;
    if (startsWithNoCase(trimmed, "there is no such"))
        return true;

    return false;
}

void BrDeath_DeathFeedLog(Player const* player,
    uint8 level,
    std::string const& guildName,
    std::string const& killerLabel,
    std::string const& lastWords)
{
    if (!player)
        return;

    std::string const name = BrDeath_SanitizeForPipeLog(player->GetName());
    std::string const guild = BrDeath_SanitizeForPipeLog(guildName);
    std::string const killer = BrDeath_SanitizeForPipeLog(killerLabel);
    std::string const words =
        BrDeath_SanitizeForPipeLog(lastWords.empty() ? "..." : lastWords);

    LOG_INFO("server.worldserver",
        "BRDEATH|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        name,
        uint32(level),
        guild,
        killer,
        player->GetMapId(),
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ(),
        words);
}

void StorePendingKiller(uint32 playerGuidLow, std::string label)
{
    g_pendingKillerByPlayerLow[playerGuidLow] = std::move(label);
}

std::string TakePendingKiller(uint32 playerGuidLow)
{
    auto it = g_pendingKillerByPlayerLow.find(playerGuidLow);
    if (it == g_pendingKillerByPlayerLow.end())
        return {};

    std::string out = std::move(it->second);
    g_pendingKillerByPlayerLow.erase(it);
    return out;
}

void StoreLastWords(uint32 playerGuidLow, std::string const& msg)
{
    if (msg.empty())
        return;

    std::string trimmed = msg;
    if (trimmed.size() > 140)
        trimmed = trimmed.substr(0, 140);

    if (BrDeath_IsNoiseLastWords(trimmed))
        return;

    g_lastWordsByPlayerLow[playerGuidLow] = trimmed;
}

std::string TakeLastWords(uint32 playerGuidLow)
{
    auto it = g_lastWordsByPlayerLow.find(playerGuidLow);
    if (it == g_lastWordsByPlayerLow.end())
        return {};

    std::string out = std::move(it->second);
    g_lastWordsByPlayerLow.erase(it);
    return out;
}

bool BrDeath_ShouldRun(Player const* player)
{
    if (!sConfigMgr->GetOption<bool>("BlackroseDeath.Enable", true))
        return false;

    if (sConfigMgr->GetOption<bool>("BlackroseDeath.Hardcore.Only", false) &&
        !sWorld->getBoolConfig(CONFIG_HARDCORE_ENABLED))
        return false;

    if (!player)
        return false;

    if (sConfigMgr->GetOption<bool>("BlackroseDeath.Skip.Battleground", true) &&
        player->InBattleground())
        return false;

    if (sConfigMgr->GetOption<bool>("BlackroseDeath.Skip.Arena", true) &&
        player->InArena())
        return false;

    if (sConfigMgr->GetOption<bool>("BlackroseDeath.Skip.Instance", false))
    {
        if (Map const* map = player->GetMap())
            if (map->Instanceable())
                return false;
    }

    return true;
}

std::string BrDeath_GuildName(Player const* player)
{
    uint32 const gid = player->GetGuildId();
    if (!gid)
        return "Unguilded";

    if (Guild const* g = sGuildMgr->GetGuildById(gid))
        return g->GetName();

    return "Unguilded";
}

std::vector<uint32> BrDeath_ParseEntries()
{
    std::string const raw = sConfigMgr->GetOption<std::string>(
        "BlackroseDeath.Gravestone.Entries", "61,148504");
    std::vector<uint32> out;
    std::stringstream ss(raw);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        uint32 entry = uint32(std::strtoul(item.c_str(), nullptr, 10));
        if (entry > 0)
            out.push_back(entry);
    }
    return out;
}

uint32 BrDeath_SelectEntry(Player const* player)
{
    std::vector<uint32> entries = BrDeath_ParseEntries();
    if (entries.empty())
        return sConfigMgr->GetOption<uint32>("BlackroseDeath.Gravestone.Entry", 61);

    std::size_t idx =
        (std::size_t(std::time(nullptr)) + player->GetGUID().GetCounter()) %
        entries.size();
    return entries[idx];
}

void BrDeath_SpawnGravestone(Player* player)
{
    if (!sConfigMgr->GetOption<bool>("BlackroseDeath.Gravestone.Enable", true))
        return;

    uint32 const entry = BrDeath_SelectEntry(player);
    int32 const duration =
        (int32)sConfigMgr->GetOption<int32>("BlackroseDeath.Gravestone.Duration", 1209600);
    if (!entry || duration <= 0)
        return;

    if (!player->IsInWorld())
        return;

    Map* map = player->GetMap();
    if (!map)
        return;

    float x = player->GetPositionX();
    float y = player->GetPositionY();
    float z = player->GetPositionZ();

    if (sConfigMgr->GetOption<bool>("BlackroseDeath.Gravestone.GroundSnap", true))
        player->UpdateGroundPositionZ(x, y, z);

    // Spawn map-owned timed GO so it survives player death-state cleanups.
    if (GameObject* go = map->SummonGameObject(entry,
            x,
            y,
            z,
            player->GetOrientation(),
            0.f,
            0.f,
            0.f,
            0.f,
            duration,
            true))
    {
        go->SetGameObjectFlag(GO_FLAG_NOT_SELECTABLE);

        if (sConfigMgr->GetOption<bool>("BlackroseDeath.Gravestone.AllPhases", true))
            go->SetPhaseMask(uint32(PHASEMASK_ANYWHERE), true);

        LOG_INFO("scripts",
            "BlackroseDeath: spawned gravestone entry {} for {} ({}) despawn in {}s",
            entry,
            player->GetName(),
            player->GetGUID().ToString(),
            duration);
    }
    else
    {
        LOG_ERROR("scripts",
            "BlackroseDeath: SummonGameObject failed entry {} for {} — check "
            "gameobject_template and BlackroseDeath.Gravestone.Entry",
            entry,
            player->GetName());
    }
}

void BrDeath_Broadcast(Player const* player,
    uint8 level,
    std::string const& guildName,
    std::string const& killerLabel,
    std::string const& lastWords)
{
    if (!sConfigMgr->GetOption<bool>("BlackroseDeath.Broadcast", true))
        return;

    std::string const name = player->GetName();
    std::string const msg =
        "[Death] " + name + " (level " + std::to_string((int)level) + ", " + guildName +
        ") was slain by " + killerLabel + ".";

    ChatHandler(nullptr).SendWorldText("{}", msg);
    ChatHandler(nullptr).SendWorldText("Last Words: {}", lastWords.empty() ? "..." : lastWords);
}
} // namespace

class blackrose_death_announce_playerscript : public PlayerScript
{
public:
    blackrose_death_announce_playerscript()
        : PlayerScript("blackrose_death_announce_playerscript",
              { PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
                  PLAYERHOOK_ON_PVP_KILL,
                  PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE,
                  PLAYERHOOK_ON_PLAYER_JUST_DIED })
    {
    }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& /*lang*/, std::string& msg) override
    {
        if (!player || msg.empty())
            return;

        switch (type)
        {
            case CHAT_MSG_SAY:
            case CHAT_MSG_YELL:
            case CHAT_MSG_CHANNEL:
            case CHAT_MSG_GUILD:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_RAID:
            case CHAT_MSG_WHISPER:
            case CHAT_MSG_EMOTE:
                StoreLastWords(player->GetGUID().GetCounter(), msg);
                break;
            default:
                break;
        }
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!BrDeath_ShouldRun(killed) || !killer || !killed)
            return;

        StorePendingKiller(killed->GetGUID().GetCounter(), killer->GetName());
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!BrDeath_ShouldRun(killed) || !killer || !killed || killer == killed)
            return;

        StorePendingKiller(killed->GetGUID().GetCounter(), killer->GetName());
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!BrDeath_ShouldRun(player))
            return;

        uint32 const low = player->GetGUID().GetCounter();
        std::string killerLabel = TakePendingKiller(low);
        if (killerLabel.empty())
            killerLabel = "Unknown";
        std::string lastWords = TakeLastWords(low);

        uint8 const level = player->GetLevel();
        std::string const guildName = BrDeath_GuildName(player);

        BrDeath_DeathFeedLog(player, level, guildName, killerLabel, lastWords);
        BrDeath_Broadcast(player, level, guildName, killerLabel, lastWords);
        BrDeath_SpawnGravestone(player);
    }
};

void AddSC_blackrose_death_announce()
{
    new blackrose_death_announce_playerscript();
}
