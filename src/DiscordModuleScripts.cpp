#include "DiscordMgr.h"
#include "DiscordAdmin.h"
#include "DiscordChatBridge.h"
#include "DiscordClient.h"
#include "DiscordEvents.h"
#include "DiscordPlayerProvider.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Channel.h"
#include "CommandScript.h"
#include "GameTime.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

using namespace Acore::ChatCommands;

// ---------------------------------------------------------------------------
// World lifecycle
// ---------------------------------------------------------------------------

class DiscordWorldScript : public WorldScript
{
public:
    DiscordWorldScript() : WorldScript("DiscordWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_SHUTDOWN_INITIATE,
        WORLDHOOK_ON_SHUTDOWN,
        WORLDHOOK_ON_UPDATE
    }) {}

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            sDiscordMgr->ReloadConfig();
        else
            sDiscordMgr->Initialize();
    }

    void OnShutdownInitiate(ShutdownExitCode /*code*/, ShutdownMask mask) override
    {
        if (sDiscordMgr->IsEnabled())
            sDiscordMgr->Events()->OnServerShutdown(mask & SHUTDOWN_MASK_RESTART);
    }

    void OnShutdown() override
    {
        sDiscordMgr->Shutdown();
    }

    void OnUpdate(uint32 diff) override
    {
        sDiscordMgr->Update(diff);
    }
};

// ---------------------------------------------------------------------------
// Player events
// ---------------------------------------------------------------------------

class DiscordPlayerScript : public PlayerScript
{
public:
    DiscordPlayerScript() : PlayerScript("DiscordPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_ACHI_COMPLETE,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT
    }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (sDiscordMgr->IsEnabled())
            sDiscordMgr->Events()->OnPlayerLogin(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (sDiscordMgr->IsEnabled())
            sDiscordMgr->Events()->OnPlayerLogout(player);
    }

    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if (sDiscordMgr->IsEnabled())
            sDiscordMgr->Events()->OnAchievementComplete(player, achievement);
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (sDiscordMgr->IsEnabled())
            sDiscordMgr->Events()->OnCreatureKill(killer, killed);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Channel* channel) override
    {
        // Capture channel-based Global Chat providers (e.g. a "World" channel).
        if (sDiscordMgr->IsEnabled() && channel)
            sDiscordMgr->ChatBridge()->HandleChannelChat(player, channel->GetName(), msg);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Global command hook (provider capture + GM/ban/mute event logging)
// ---------------------------------------------------------------------------

class DiscordAllCommandScript : public AllCommandScript
{
public:
    DiscordAllCommandScript() : AllCommandScript("DiscordAllCommandScript", {
        ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND
    }) {}

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
    {
        if (!sDiscordMgr->IsEnabled())
            return true;

        // Capture in-game Global Chat messages (e.g. ".chat hello").
        if (handler.GetSession() && handler.GetSession()->GetPlayer())
            sDiscordMgr->ChatBridge()->HandleGameCommand(handler.GetSession()->GetPlayer(), cmdStr);

        // Log GM commands / bans / mutes to the events channel.
        sDiscordMgr->Events()->OnCommandExecuted(&handler, cmdStr);

        return true;
    }
};

// ---------------------------------------------------------------------------
// In-game .discord command
// ---------------------------------------------------------------------------

class discord_commandscript : public CommandScript
{
public:
    discord_commandscript() : CommandScript("discord_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable discordSubCommands =
        {
            { "status",     HandleDiscordStatus,     SEC_PLAYER,     Console::No },
            { "population", HandleDiscordPopulation, SEC_PLAYER,     Console::No },
            { "uptime",     HandleDiscordUptime,     SEC_PLAYER,     Console::No },
            { "version",    HandleDiscordVersion,    SEC_PLAYER,     Console::No },
            { "setup",      HandleDiscordSetup,      SEC_GAMEMASTER, Console::No },
            { "reload",     HandleDiscordReload,     SEC_GAMEMASTER, Console::No },
            { "say",        HandleDiscordSay,        SEC_PLAYER,     Console::No },
            { "",           HandleDiscordHelp,       SEC_PLAYER,     Console::No }
        };
        static ChatCommandTable commandTable =
        {
            { "discord", discordSubCommands }
        };
        return commandTable;
    }

    static bool RequireEnabled(ChatHandler* handler)
    {
        if (!sDiscordMgr->IsEnabled())
        {
            handler->SendSysMessage("Discord integration is disabled on this server.");
            return false;
        }
        return true;
    }

    static bool HandleDiscordStatus(ChatHandler* handler)
    {
        if (!RequireEnabled(handler))
            return true;

        DiscordPopulation pop = sDiscordMgr->PlayerProvider()->GetPopulation();
        std::string realm = sDiscordMgr->Config()->ServerName.empty()
            ? "AzerothCore" : sDiscordMgr->Config()->ServerName;

        std::string text = "Status: " + std::string(sDiscordMgr->Client() && sDiscordMgr->Client()->IsConnected()
            ? "Online" : "Offline") + "\n";
        text += "Realm: " + realm + "\n";
        text += "Players: " + std::to_string(pop.real) + "\n";
        if (sDiscordMgr->Config()->ShowPopulation && pop.bots > 0)
            text += "Playerbots: " + std::to_string(pop.bots) + "\nTotal: " + std::to_string(pop.total) + "\n";
        text += "Uptime: " + sDiscordMgr->FormatUptime(uint32(GameTime::GetUptime().count())) + "\n";
        text += "Expansion: WotLK 3.3.5a";
        handler->SendSysMessage(text);
        return true;
    }

    static bool HandleDiscordPopulation(ChatHandler* handler)
    {
        if (!RequireEnabled(handler))
            return true;
        DiscordPopulation pop = sDiscordMgr->PlayerProvider()->GetPopulation();
        if (sDiscordMgr->Config()->ShowPopulation)
            handler->SendSysMessage("Players: " + std::to_string(pop.real) + " | Playerbots: " +
                                    std::to_string(pop.bots) + " | Total: " + std::to_string(pop.total));
        else
            handler->SendSysMessage("Players: " + std::to_string(pop.real));
        return true;
    }

    static bool HandleDiscordUptime(ChatHandler* handler)
    {
        if (!RequireEnabled(handler))
            return true;
        handler->SendSysMessage("Uptime: " + sDiscordMgr->FormatUptime(uint32(GameTime::GetUptime().count())));
        return true;
    }

    static bool HandleDiscordVersion(ChatHandler* handler)
    {
        if (!RequireEnabled(handler))
            return true;
        handler->SendSysMessage("Realm: " + sDiscordMgr->Config()->ServerName);
        handler->SendSysMessage("World DB: " + std::string(sWorld->GetDBVersion() ? sWorld->GetDBVersion() : "unknown"));
        handler->SendSysMessage("Expansion: WotLK 3.3.5a | Module: mod-discord-chat");
        return true;
    }

    static bool HandleDiscordReload(ChatHandler* handler)
    {
        if (!RequireEnabled(handler))
            return true;
        sWorld->LoadConfigSettings(true);
        handler->SendSysMessage("Discord module configuration reloaded.");
        return true;
    }

    static bool HandleDiscordSetup(ChatHandler* handler, Tail args)
    {
        if (!RequireEnabled(handler))
            return true;
        std::string sub(args);
        size_t space = sub.find(' ');
        if (space != std::string::npos)
            sub = sub.substr(0, space);
        if (sub != "roles")
        {
            handler->SendSysMessage("Usage: .discord setup roles");
            return true;
        }
        sDiscordMgr->Admin()->CreateRolesInGame(handler);
        return true;
    }

    static bool HandleDiscordSay(ChatHandler* handler, Tail args)
    {
        if (!RequireEnabled(handler))
            return true;
        std::string message(args);
        if (message.find_first_not_of(' ') == std::string::npos)
        {
            handler->SendSysMessage("Usage: .discord say <message>");
            return true;
        }
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return true;
        sDiscordMgr->ChatBridge()->HandleDiscordSay(player, message);
        handler->SendSysMessage("Message relayed to Discord.");
        return true;
    }

    static bool HandleDiscordHelp(ChatHandler* handler)
    {
        if (!RequireEnabled(handler))
            return true;
        handler->SendSysMessage("Discord integration commands:");
        handler->SendSysMessage("  .discord status     - show server status");
        handler->SendSysMessage("  .discord population - show player population");
        handler->SendSysMessage("  .discord uptime     - show server uptime");
        handler->SendSysMessage("  .discord version    - show version info");
        handler->SendSysMessage("  .discord setup roles - create Discord roles (GM)");
        handler->SendSysMessage("  .discord reload     - reload module config (GM)");
        handler->SendSysMessage("  .discord say <msg>  - relay a message to Discord");
        return true;
    }
};

void AddDiscordChatScripts()
{
    new DiscordWorldScript();
    new DiscordPlayerScript();
    new DiscordAllCommandScript();
    new discord_commandscript();
}