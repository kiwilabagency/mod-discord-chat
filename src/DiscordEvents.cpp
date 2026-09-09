#include "DiscordEvents.h"
#include "DiscordClient.h"
#include "DiscordConfig.h"
#include "DiscordJson.h"
#include "DiscordMgr.h"
#include "DiscordPlayerProvider.h"
#include "Chat.h"
#include "Creature.h"
#include "GameTime.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

bool DiscordEvents::Enabled() const
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    return cfg && cfg->EventsEnable && (cfg->EventsChannelId || !cfg->WebhookEvents.empty());
}

void DiscordEvents::Initialize()
{
    initialized_ = true;
}

void DiscordEvents::Shutdown()
{
    initialized_ = false;
}

void DiscordEvents::PostEvent(std::string const& text, uint32 color, bool asEmbed)
{
    if (!initialized_ || text.empty())
        return;

    DiscordConfig const* cfg = sDiscordMgr->Config();
    auto client = sDiscordMgr->Client();
    if (!cfg || !client)
        return;

    std::string json;
    if (asEmbed)
    {
        DiscordJson::Writer w;
        w.Open();
        w.Key("embeds");
        w.out += "[";
        DiscordJson::Writer e;
        e.Open();
        e.Field("description", text);
        if (color)
            e.Field("color", (long long)color);
        e.Close();
        w.out += e.out;
        w.out += "]";
        w.Close();
        json = w.out;
    }
    else
    {
        DiscordJson::Writer w;
        w.Open();
        w.Field("content", text);
        w.Close();
        json = w.out;
    }

    if (!cfg->WebhookEvents.empty())
        client->PostWebhook(cfg->WebhookEvents, json);
    else if (cfg->EventsChannelId)
        client->PostChannelMessage(cfg->EventsChannelId, json);
}

void DiscordEvents::OnPlayerLogin(Player* player)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->EventPlayerLogin || !Enabled())
        return;
    if (sDiscordMgr->PlayerProvider()->IsPlayerBot(player) && !cfg->IncludeBotsInEvents)
        return;
    PostEvent("**" + std::string(player->GetName()) + "** joined the realm.");
}

void DiscordEvents::OnPlayerLogout(Player* player)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->EventPlayerLogout || !Enabled())
        return;
    if (sDiscordMgr->PlayerProvider()->IsPlayerBot(player) && !cfg->IncludeBotsInEvents)
        return;
    PostEvent("**" + std::string(player->GetName()) + "** left the realm.");
}

void DiscordEvents::OnAchievementComplete(Player* player, AchievementEntry const* achievement)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->EventAchievement || !Enabled() || !achievement)
        return;
    if (achievement->points < uint32(cfg->AchievementMinValue))
        return;
    if (sDiscordMgr->PlayerProvider()->IsPlayerBot(player) && !cfg->IncludeBotsInEvents)
        return;

    LocaleConstant locale = sWorld->GetDefaultDbcLocale();
    std::string name = achievement->name[locale] ? achievement->name[locale] : "an achievement";
    PostEvent("**" + std::string(player->GetName()) + "** earned [" + name + "].",
              DiscordChat::ClassColor(player->getClass()), true);
}

bool DiscordEvents::IsBossKillEnabled(Creature* killed) const
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->EventBossKill)
        return false;

    if (killed->GetMap()->IsBattlegroundOrArena())
        return false;

    uint32 rank = killed->GetCreatureTemplate()->rank;

    // World boss (outdoor, no instance).
    if (killed->GetMap()->IsWorldMap())
    {
        if (rank != CREATURE_ELITE_WORLDBOSS)
            return false;
        return cfg->BossWorld;
    }

    // Instance bosses.
    bool isRaid = killed->GetMap()->IsRaid();
    if (isRaid)
        return cfg->BossRaid;
    if (killed->GetMap()->IsDungeon())
        return cfg->BossDungeon;

    // Outdoor bosses that are not on a world map (rare edge cases).
    if (rank == CREATURE_ELITE_WORLDBOSS)
        return cfg->BossWorld;
    return false;
}

void DiscordEvents::OnCreatureKill(Player* killer, Creature* killed)
{
    if (!initialized_ || !Enabled() || !killer || !killed)
        return;
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->EventBossKill)
        return;

    if (!IsBossKillEnabled(killed))
        return;

    if (sDiscordMgr->PlayerProvider()->IsPlayerBot(killer) && !cfg->IncludeBotsInEvents)
        return;

    std::string bossName = killed->GetName();
    if (bossName.empty())
        return;

    // Optional final-boss-only filtering (heuristic: no other undefeated bosses
    // remain in the instance after this kill).
    if (cfg->BossFinalBossOnly)
    {
        InstanceMap* instanceMap = killed->GetMap()->ToInstanceMap();
        InstanceScript* script = instanceMap ? instanceMap->GetInstanceScript() : nullptr;
        if (!script)
            return;
        bool allDone = true;
        for (uint8 i = 0; i < script->GetEncounterCount(); ++i)
            if (script->GetBossState(i) != EncounterState::DONE)
            {
                allDone = false;
                break;
            }
        if (!allDone)
            return;
    }

    PostEvent("**" + std::string(killer->GetName()) + "**'s group defeated " + bossName + ".",
              DiscordChat::ClassColor(killer->getClass()));
}

void DiscordEvents::OnCommandExecuted(ChatHandler* handler, std::string_view cmdStr)
{
    if (!initialized_ || !Enabled())
        return;
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg)
        return;

    std::string cmd(cmdStr);
    std::string first = cmd.substr(0, cmd.find(' '));

    bool isBan = first == "ban" || first == "unban";
    bool isMute = first == "mute" || first == "unmute";

    // Ban / mute events.
    if (isBan || isMute)
    {
        bool enabled = (isBan && cfg->EventBan) || (isMute && cfg->EventMute);
        if (enabled)
        {
            std::string who = handler && handler->GetSession() && handler->GetSession()->GetPlayer()
                ? handler->GetSession()->GetPlayer()->GetName()
                : std::string("console");
            PostEvent("**" + who + "** ran: `" + cmd + "`");
        }
        return;
    }

    // General GM command logging (only for accounts with GM+ security).
    // Global chat / bridge commands are already relayed as chat; never log them
    // as GM activity (avoids duplicating messages when the events channel and
    // the chat channel are the same).
    if (first == "chat" || first == "global" || first == "world" || first == "discord")
        return;

    if (cfg->EventGmCommand && handler && handler->GetSession() && handler->GetSession()->GetPlayer() &&
        handler->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
    {
        PostEvent("**" + std::string(handler->GetSession()->GetPlayer()->GetName()) + "** ran: `" + cmd + "`");
    }
}

void DiscordEvents::OnMemberJoin(std::string const& userName, uint64_t userId)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->EventMemberJoin || !Enabled())
        return;
    PostEvent("**" + userName + "** joined the Discord server. (<@" + std::to_string(userId) + ">)");
}

void DiscordEvents::OnMemberLeave(std::string const& userName, uint64_t userId)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->EventMemberLeave || !Enabled())
        return;
    PostEvent("**" + userName + "** left the Discord server. (" + std::to_string(userId) + ")");
}

void DiscordEvents::OnServerStartup()
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->EventServerStartup || !Enabled())
        return;
    PostEvent(":green_circle: " + cfg->ServerName + " has started.", 0x00AE86, true);
}

void DiscordEvents::OnServerShutdown(bool restart)
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !Enabled())
        return;
    if (restart && cfg->EventServerRestart)
        PostEvent(":yellow_circle: " + cfg->ServerName + " is restarting.", 0xFEE75C, true);
    else if (!restart && cfg->EventServerShutdown)
        PostEvent(":red_circle: " + cfg->ServerName + " is shutting down.", 0xED4245, true);
}