#include "DiscordAdmin.h"
#include "DiscordClient.h"
#include "DiscordConfig.h"
#include "DiscordJson.h"
#include "DiscordMgr.h"
#include "DiscordChatBridge.h"
#include "DiscordEmojiManager.h"
#include "DiscordPlayerProvider.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "Random.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cstdlib>
#include <deque>

namespace
{
    std::string EscapeSql(std::string const& v)
    {
        std::string out;
        out.reserve(v.size());
        for (char c : v)
        {
            if (c == '\'')
                out += "''";
            else
                out += c;
        }
        return out;
    }

    std::string MakeJsonString(std::string const& v)
    {
        return "\"" + DiscordJson::Escape(v) + "\"";
    }

    // Command registration payload (all guild commands).
    char const* kCommandsJson = R"json([
  {"name":"server","description":"Server administration","options":[
    {"name":"status","description":"Show server status","type":1},
    {"name":"population","description":"Show player population","type":1},
    {"name":"uptime","description":"Show server uptime","type":1},
    {"name":"version","description":"Show server version","type":1},
    {"name":"restart","description":"Restart the server after a delay","type":1,"options":[
      {"name":"delay","description":"Delay in seconds","type":4,"required":true},
      {"name":"reason","description":"Reason","type":3,"required":false}]},
    {"name":"shutdown","description":"Shutdown the server after a delay","type":1,"options":[
      {"name":"delay","description":"Delay in seconds","type":4,"required":true},
      {"name":"reason","description":"Reason","type":3,"required":false}]},
    {"name":"cancel","description":"Cancel a pending shutdown or restart","type":1}]},
  {"name":"player","description":"Player administration","options":[
    {"name":"info","description":"Show player information","type":1,"options":[
      {"name":"player","description":"Player name","type":3,"required":true}]},
    {"name":"kick","description":"Kick a player from the server","type":1,"options":[
      {"name":"player","description":"Player name","type":3,"required":true},
      {"name":"reason","description":"Reason","type":3,"required":false}]},
    {"name":"mute","description":"Mute a player","type":1,"options":[
      {"name":"player","description":"Player name","type":3,"required":true},
      {"name":"minutes","description":"Mute duration in minutes","type":4,"required":true},
      {"name":"reason","description":"Reason","type":3,"required":false}]},
    {"name":"unmute","description":"Unmute a player","type":1,"options":[
      {"name":"player","description":"Player name","type":3,"required":true}]},
    {"name":"teleport","description":"Teleport a player to a location","type":1,"options":[
      {"name":"player","description":"Player name","type":3,"required":true},
      {"name":"location","description":"Named teleport location","type":3,"required":true}]},
    {"name":"summon","description":"Summon a player to another player or location","type":1,"options":[
      {"name":"player","description":"Player name","type":3,"required":true},
      {"name":"target","description":"Destination player (optional)","type":3,"required":false},
      {"name":"location","description":"Named teleport location (optional)","type":3,"required":false}]},
    {"name":"level","description":"Set a player's level","type":1,"options":[
      {"name":"player","description":"Player name","type":3,"required":true},
      {"name":"level","description":"New level","type":4,"required":true}]},
    {"name":"money","description":"Set a player's money (copper)","type":1,"options":[
      {"name":"player","description":"Player name","type":3,"required":true},
      {"name":"amount","description":"Money in copper","type":4,"required":true}]},
    {"name":"item","description":"Grant an item to a player","type":1,"options":[
      {"name":"player","description":"Player name","type":3,"required":true},
      {"name":"item","description":"Item entry id","type":4,"required":true},
      {"name":"count","description":"Count (default 1)","type":4,"required":false}]}]},
  {"name":"account","description":"Account administration","options":[
    {"name":"info","description":"Show account information","type":1,"options":[
      {"name":"account","description":"Account or player name","type":3,"required":true}]},
    {"name":"ban","description":"Ban an account","type":1,"options":[
      {"name":"account","description":"Account name","type":3,"required":true},
      {"name":"days","description":"Ban length in days","type":4,"required":true},
      {"name":"reason","description":"Reason","type":3,"required":false}]},
    {"name":"unban","description":"Unban an account","type":1,"options":[
      {"name":"account","description":"Account name","type":3,"required":true}]},
    {"name":"mute","description":"Mute an account","type":1,"options":[
      {"name":"account","description":"Account name","type":3,"required":true},
      {"name":"minutes","description":"Mute duration in minutes","type":4,"required":true}]},
    {"name":"unmute","description":"Unmute an account","type":1,"options":[
      {"name":"account","description":"Account name","type":3,"required":true}]}]},
  {"name":"announce","description":"Send announcements","options":[
    {"name":"global","description":"Yellow global announcement","type":1,"options":[
      {"name":"message","description":"Message","type":3,"required":true}]},
    {"name":"notification","description":"Red center-screen notification","type":1,"options":[
      {"name":"message","description":"Message","type":3,"required":true}]},
    {"name":"server","description":"Server-style message","type":1,"options":[
      {"name":"message","description":"Message","type":3,"required":true}]}]},
  {"name":"config","description":"Configuration management","options":[
    {"name":"reload","description":"Reload module configuration","type":1}]},
  {"name":"setup","description":"One-time server setup helpers","options":[
    {"name":"roles","description":"Create the recommended class/race/faction/admin roles","type":1}]},
  {"name":"discord","description":"Discord integration commands","options":[
    {"name":"emojis","description":"Emoji management","type":1,"options":[
      {"name":"reload","description":"Reload the guild emoji cache","type":1}]}]},
  {"name":"console","description":"Run a raw worldserver command (disabled by default)","options":[
    {"name":"command","description":"Command to execute","type":3,"required":true}]}
])json";
}

void DiscordAdmin::Initialize()
{
    initialized_ = true;
    pendingActions_.clear();
}

void DiscordAdmin::Shutdown()
{
    initialized_ = false;
    pendingActions_.clear();
}

uint32 DiscordAdmin::NowSeconds() const
{
    return uint32(GameTime::GetUptime().count());
}

std::string DiscordAdmin::MakeToken()
{
    std::string tok = std::to_string(getMSTime());
    for (int i = 0; i < 4; ++i)
        tok += char('a' + urand(0, 25));
    return tok;
}

void DiscordAdmin::RegisterCommands()
{
    auto client = sDiscordMgr->Client();
    if (!client)
        return;
    client->RegisterCommands(kCommandsJson);
}

// ---------------------------------------------------------------------------
// Reply helpers
// ---------------------------------------------------------------------------

void DiscordAdmin::PostInteraction(DiscordInteraction const& ix, std::string const& json)
{
    DiscordOutMessage msg;
    msg.payload = json;
    msg.path = "/interactions/" + std::to_string(ix.id) + "/" + ix.token + "/callback";
    msg.method = "POST";
    msg.needsAuth = true;
    GetDiscordQuickSender().Send(std::move(msg));
}

void DiscordAdmin::Reply(DiscordInteraction const& ix, std::string const& content, bool ephemeral)
{
    DiscordJson::Writer w;
    w.Open();
    w.Field("type", 4LL);
    w.Key("data");
    w.Open();
    w.Field("content", content);
    if (ephemeral)
        w.Field("flags", 64LL);
    w.Close();
    w.Close();
    PostInteraction(ix, w.out);
}

void DiscordAdmin::ReplyEmbed(DiscordInteraction const& ix, std::string const& title,
                              std::string const& description, uint32 color, bool ephemeral)
{
    DiscordJson::Writer w;
    w.Open();
    w.Field("type", 4LL);
    w.Key("data");
    w.Open();
    w.Key("embeds");
    w.out += "[";
    DiscordJson::Writer e;
    e.Open();
    e.Field("title", title);
    e.Field("description", description);
    e.Field("color", (long long)color);
    e.Close();
    w.out += e.out;
    w.out += "]";
    if (ephemeral)
        w.Field("flags", 64LL);
    w.Close();
    w.Close();
    PostInteraction(ix, w.out);
}

void DiscordAdmin::UpdateMessage(DiscordInteraction const& ix, std::string const& content)
{
    DiscordJson::Writer w;
    w.Open();
    w.Field("type", 7LL);
    w.Key("data");
    w.Open();
    w.Field("content", content);
    w.Raw("components", "[]");
    w.Close();
    w.Close();
    PostInteraction(ix, w.out);
}

void DiscordAdmin::UpdateMessageWithComponents(DiscordInteraction const& ix, std::string const& content,
                                               std::string const& confirmId, std::string const& cancelId)
{
    DiscordJson::Writer w;
    w.Open();
    w.Field("type", 7LL);
    w.Key("data");
    w.Open();
    w.Field("content", content);
    w.Key("components");
    w.out += "[{\"type\":1,\"components\":["
             "{\"type\":2,\"style\":4,\"label\":\"Confirm\",\"custom_id\":" + MakeJsonString(confirmId) + "},"
             "{\"type\":2,\"style\":2,\"label\":\"Cancel\",\"custom_id\":" + MakeJsonString(cancelId) + "}]}]";
    w.Close();
    w.Close();
    PostInteraction(ix, w.out);
}

// ---------------------------------------------------------------------------
// Permissions
// ---------------------------------------------------------------------------

bool DiscordAdmin::IsAdmin(DiscordInteraction const& ix) const
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->AdminEnable)
        return false;
    return cfg->IsAdminAuthorized(ix.userId, ix.roles);
}

bool DiscordAdmin::CategoryEnabled(char const* group) const
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg)
        return false;
    if (std::string(group) == "server") return cfg->CmdServer;
    if (std::string(group) == "player") return cfg->CmdPlayer;
    if (std::string(group) == "account") return cfg->CmdAccount;
    if (std::string(group) == "announce") return cfg->CmdAnnounce;
    if (std::string(group) == "config") return cfg->CmdConfig;
    if (std::string(group) == "setup") return cfg->CmdConfig;
    if (std::string(group) == "console") return cfg->CmdConsole && cfg->AllowRawConsole;
    return false;
}

std::string DiscordAdmin::FormatError(std::string const& e) const
{
    return "Error: " + e;
}

// ---------------------------------------------------------------------------
// Interaction entry point
// ---------------------------------------------------------------------------

void DiscordAdmin::HandleInteraction(DiscordInteraction const& ix)
{
    if (!initialized_)
        return;

    // Message component (button) interactions.
    if (!ix.options.empty() && ix.options.front().name == "__component__")
    {
        HandleComponentInteraction(ix);
        return;
    }

    // Admin commands: enforce admin channel + authorization + category toggles.
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->AdminEnable)
    {
        Reply(ix, "Discord admin commands are disabled on this server.", true);
        return;
    }
    if (cfg->AdminChannelId && ix.channelId != cfg->AdminChannelId)
    {
        Reply(ix, "Admin commands are only allowed in the configured admin channel.", true);
        return;
    }
    if (!IsAdmin(ix))
    {
        Reply(ix, "You are not authorized to use admin commands. Contact a server administrator.", true);
        return;
    }
    // Category gate.
    std::string group = ix.name;
    if (!CategoryEnabled(group.c_str()))
    {
        Reply(ix, "The '" + group + "' command category is disabled on this server.", true);
        return;
    }

    HandleCommand(ix);
}

void DiscordAdmin::HandleCommand(DiscordInteraction const& ix)
{
    if (ix.name == "server") HandleServerCommands(ix);
    else if (ix.name == "player") HandlePlayerCommands(ix);
    else if (ix.name == "account") HandleAccountCommands(ix);
    else if (ix.name == "announce") HandleAnnounceCommands(ix);
    else if (ix.name == "config") HandleConfigCommands(ix);
    else if (ix.name == "console") HandleConsoleCommand(ix);
    else if (ix.name == "discord") HandleDiscordCommands(ix);
    else if (ix.name == "setup") HandleSetupCommands(ix);
    else Reply(ix, "Unknown command: " + ix.name, true);
}

// ---------------------------------------------------------------------------
// Confirmation flow
// ---------------------------------------------------------------------------

void DiscordAdmin::RequestConfirmation(DiscordInteraction const& ix, std::string const& command,
                                       std::string const& args, std::string const& detail)
{
    std::string token = MakeToken();
    PendingAction action;
    action.token = token;
    action.requesterId = ix.userId;
    action.command = command;
    action.args = args;
    action.createdAt = NowSeconds();
    pendingActions_[token] = action;

    std::string content = "**" + command + "** requested by <@" + std::to_string(ix.userId) + ">\n" +
                          "`" + args + "`\n" + detail + "\n\nConfirm to execute, or Cancel to abort.";

    DiscordJson::Writer w;
    w.Open();
    w.Field("type", 4LL);
    w.Key("data");
    w.Open();
    w.Field("content", content);
    w.Key("components");
    w.out += "[{\"type\":1,\"components\":["
             "{\"type\":2,\"style\":4,\"label\":\"Confirm\",\"custom_id\":" + MakeJsonString("dc-" + token + "-confirm") + "},"
             "{\"type\":2,\"style\":2,\"label\":\"Cancel\",\"custom_id\":" + MakeJsonString("dc-" + token + "-cancel") + "}]}]";
    w.Close();
    w.Close();
    PostInteraction(ix, w.out);
}

void DiscordAdmin::HandleComponentInteraction(DiscordInteraction const& ix)
{
    std::string customId = ix.options.front().value;
    std::string token;
    bool confirm = false;
    if (customId.rfind("dc-", 0) == 0)
    {
        std::string rest = customId.substr(3);
        size_t sep = rest.rfind("-confirm");
        if (sep != std::string::npos)
        {
            token = rest.substr(0, sep);
            confirm = true;
        }
        else
        {
            sep = rest.rfind("-cancel");
            if (sep != std::string::npos)
            {
                token = rest.substr(0, sep);
                confirm = false;
            }
        }
    }

    auto it = pendingActions_.find(token);
    if (it == pendingActions_.end())
    {
        Reply(ix, "This action no longer exists or has already been handled.", true);
        return;
    }
    PendingAction action = it->second;

    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (NowSeconds() - action.createdAt > (cfg ? cfg->ConfirmTimeout : 60))
    {
        pendingActions_.erase(it);
        UpdateMessage(ix, "This action expired before confirmation.");
        return;
    }

    // Only the requester may confirm when SelfConfirmOnly is on.
    if (cfg && cfg->SelfConfirmOnly && ix.userId != action.requesterId)
    {
        Reply(ix, "Only the user who requested this action may confirm it.", true);
        return;
    }
    if (cfg && !cfg->SelfConfirmOnly && !IsAdmin(ix))
    {
        Reply(ix, "You are not authorized to confirm this action.", true);
        return;
    }

    if (confirm)
    {
        pendingActions_.erase(it);
        ExecuteAction(action, ix);
    }
    else
    {
        pendingActions_.erase(it);
        UpdateMessage(ix, "Action cancelled by <@" + std::to_string(ix.userId) + ">: " + action.command);
    }
}

// ---------------------------------------------------------------------------
// Action execution
// ---------------------------------------------------------------------------

void DiscordAdmin::ExecuteAction(PendingAction const& action, DiscordInteraction const& confirmer)
{
    if (action.command.rfind("server.", 0) == 0)
        ExecuteServerAction(action, confirmer);
    else if (action.command.rfind("player.", 0) == 0)
        ExecutePlayerAction(action, confirmer);
    else if (action.command.rfind("account.", 0) == 0)
        ExecuteAccountAction(action, confirmer);
    else if (action.command == "console")
        ExecuteConsoleAction(action, confirmer);
    else
        UpdateMessage(confirmer, "Unknown action: " + action.command);
}

void DiscordAdmin::ExecuteServerAction(PendingAction const& action, DiscordInteraction const& confirmer)
{
    std::string result;
    std::string cmd = action.command.substr(7); // "restart"/"shutdown"
    uint32 delay = 30;
    std::string reason;
    if (!action.args.empty())
    {
        // args format: "<delay>|<reason>"
        size_t sep = action.args.find('|');
        if (sep != std::string::npos)
        {
            delay = std::max<uint32>(1, std::stoul(action.args.substr(0, sep)));
            reason = action.args.substr(sep + 1);
        }
        else
            delay = std::max<uint32>(1, std::stoul(action.args));
    }

    if (cmd == "restart")
    {
        sWorld->ShutdownServ(delay, SHUTDOWN_MASK_RESTART, RESTART_EXIT_CODE, reason);
        result = "Server restart scheduled in " + std::to_string(delay) + " seconds.";
    }
    else
    {
        sWorld->ShutdownServ(delay, 0, SHUTDOWN_EXIT_CODE, reason);
        result = "Server shutdown scheduled in " + std::to_string(delay) + " seconds.";
    }
    UpdateMessage(confirmer, "Executed by <@" + std::to_string(confirmer.userId) + ">: **" + action.command +
                             "**\n" + result);
    LogAudit(confirmer, action.command, action.args, result);
}

void DiscordAdmin::ExecutePlayerAction(PendingAction const& action, DiscordInteraction const& confirmer)
{
    std::string sub = action.command.substr(7); // "level"/"money"/"item"
    std::string result;

    // args format for these: "<player>|<value>[|<count>]"
    size_t p1 = action.args.find('|');
    std::string name = p1 == std::string::npos ? action.args : action.args.substr(0, p1);
    std::string rest = p1 == std::string::npos ? "" : action.args.substr(p1 + 1);
    size_t p2 = rest.find('|');
    std::string value = p2 == std::string::npos ? rest : rest.substr(0, p2);
    std::string count = p2 == std::string::npos ? "1" : rest.substr(p2 + 1);

    Player* player = ObjectAccessor::FindPlayerByName(name, false);
    if (!player)
    {
        result = "Player '" + name + "' is not online.";
        UpdateMessage(confirmer, "Executed by <@" + std::to_string(confirmer.userId) + ">: **" + action.command +
                                 "**\n" + result);
        LogAudit(confirmer, action.command, action.args, "failed: player offline");
        return;
    }

    if (sub == "level")
    {
        uint8 level = std::clamp<uint8>(uint8(std::stoul(value)), 1, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        player->GiveLevel(level);
        player->InitTalentForLevel();
        player->SetUInt32Value(PLAYER_XP, 0);
        result = "Set " + name + " level to " + std::to_string(level) + ".";
    }
    else if (sub == "money")
    {
        int64 amount = std::stoll(value);
        player->ModifyMoney(amount);
        result = "Set " + name + " money to " + value + " copper.";
    }
    else if (sub == "item")
    {
        uint32 itemId = uint32(std::stoul(value));
        uint32 cnt = std::max<uint32>(1, uint32(std::stoul(count)));
        bool ok = player->AddItem(itemId, cnt);
        result = ok ? ("Granted " + count + "x item " + std::to_string(itemId) + " to " + name + ".")
                    : ("Failed to grant item " + std::to_string(itemId) + " (bags full?).");
    }

    UpdateMessage(confirmer, "Executed by <@" + std::to_string(confirmer.userId) + ">: **" + action.command +
                             "**\n" + result);
    LogAudit(confirmer, action.command, action.args, result);
}

void DiscordAdmin::ExecuteAccountAction(PendingAction const& action, DiscordInteraction const& confirmer)
{
    std::string sub = action.command.substr(8); // "ban"/"unban"
    std::string result;

    size_t p1 = action.args.find('|');
    std::string name = p1 == std::string::npos ? action.args : action.args.substr(0, p1);
    std::string rest = p1 == std::string::npos ? "" : action.args.substr(p1 + 1);
    size_t p2 = rest.find('|');
    std::string value = p2 == std::string::npos ? rest : rest.substr(0, p2);
    std::string reason = p2 == std::string::npos ? "Discord admin action" : rest.substr(p2 + 1);

    if (sub == "ban")
    {
        uint32 days = std::max<uint32>(1, std::stoul(value));
        std::string cmd = ".ban account " + name + " " + std::to_string(days) + "d " + reason;
        sWorld->QueueCliCommand(new CliCommandHolder(nullptr, cmd.c_str(),
                                                     [](void*, std::string_view out) {
                                                         LOG_INFO("modules.discord.admin", "console: {}", out);
                                                     },
                                                     [](void*, bool) {}));
        result = "Ban scheduled for account '" + name + "' (" + std::to_string(days) + " days).";
    }
    else
    {
        std::string cmd = ".unban account " + name;
        sWorld->QueueCliCommand(new CliCommandHolder(nullptr, cmd.c_str(),
                                                     [](void*, std::string_view out) {
                                                         LOG_INFO("modules.discord.admin", "console: {}", out);
                                                     },
                                                     [](void*, bool) {}));
        result = "Unban scheduled for account '" + name + "'.";
    }

    UpdateMessage(confirmer, "Executed by <@" + std::to_string(confirmer.userId) + ">: **" + action.command +
                             "**\n" + result);
    LogAudit(confirmer, action.command, action.args, result);
}

void DiscordAdmin::ExecuteConsoleAction(PendingAction const& action, DiscordInteraction const& confirmer)
{
    std::string cmd = action.args;
    sWorld->QueueCliCommand(new CliCommandHolder(nullptr, cmd.c_str(),
                                                 [](void*, std::string_view out) {
                                                     LOG_INFO("modules.discord.admin", "console: {}", out);
                                                 },
                                                 [](void*, bool) {}));
    std::string result = "Executed console command: " + cmd;
    UpdateMessage(confirmer, "Executed by <@" + std::to_string(confirmer.userId) + ">: **console**\n`" + cmd + "`");
    LogAudit(confirmer, "console", cmd, result);
}

// ---------------------------------------------------------------------------
// Command groups
// ---------------------------------------------------------------------------

void DiscordAdmin::HandleServerCommands(DiscordInteraction const& ix)
{
    auto sub = ix.FindSubcommand("status");
    if (!sub) sub = ix.FindSubcommand("population");
    if (!sub) sub = ix.FindSubcommand("uptime");
    if (!sub) sub = ix.FindSubcommand("version");
    if (sub)
    {
        std::string subName = sub->name;
        if (subName == "status")
        {
            std::string text = sDiscordMgr->BuildStatusText(true);
            ReplyEmbed(ix, sDiscordMgr->Config()->ServerName + " - Server Status", text, 0x00AE86);
            LogAudit(ix, "server.status", "", "ok");
            return;
        }
        if (subName == "population")
        {
            DiscordPopulation pop = sDiscordMgr->PlayerProvider()->GetPopulation();
            std::string text;
            if (sDiscordMgr->Config()->ShowPopulation)
                text = "**Players:** " + std::to_string(pop.real) + "\n" +
                       "**Playerbots:** " + std::to_string(pop.bots) + "\n" +
                       "**Total:** " + std::to_string(pop.total);
            else
                text = "**Players:** " + std::to_string(pop.real);
            ReplyEmbed(ix, "Population", text, 0x5865F2);
            LogAudit(ix, "server.population", "", "ok");
            return;
        }
        if (subName == "uptime")
        {
            std::string text = "Uptime: " + sDiscordMgr->FormatUptime(uint32(GameTime::GetUptime().count()));
            ReplyEmbed(ix, "Uptime", text, 0x5865F2);
            LogAudit(ix, "server.uptime", "", "ok");
            return;
        }
        if (subName == "version")
        {
            std::string text = "**Realm:** " + sDiscordMgr->Config()->ServerName + "\n" +
                               "**World DB:** " + (sWorld->GetDBVersion() ? sWorld->GetDBVersion() : "unknown") + "\n" +
                               "**Expansion:** WotLK 3.3.5a\n" +
                               "**Module:** mod-discord-chat";
            ReplyEmbed(ix, "Server Version", text, 0x5865F2);
            LogAudit(ix, "server.version", "", "ok");
            return;
        }
    }

    if (auto sub = ix.FindSubcommand("restart"))
    {
        std::string delay = ix.GetOptionValue(*sub, "delay");
        std::string reason = ix.GetOptionValue(*sub, "reason");
        if (delay.empty()) { Reply(ix, "Missing delay.", true); return; }
        std::string args = delay + "|" + reason;
        RequestConfirmation(ix, "server.restart", "restart in " + delay + "s" + (reason.empty() ? "" : " - " + reason),
                            "This will restart the worldserver.");
        return;
    }
    if (auto sub = ix.FindSubcommand("shutdown"))
    {
        std::string delay = ix.GetOptionValue(*sub, "delay");
        std::string reason = ix.GetOptionValue(*sub, "reason");
        if (delay.empty()) { Reply(ix, "Missing delay.", true); return; }
        RequestConfirmation(ix, "server.shutdown", "shutdown in " + delay + "s" + (reason.empty() ? "" : " - " + reason),
                            "This will shut down the worldserver.");
        return;
    }
    if (auto sub = ix.FindSubcommand("cancel"))
    {
        if (sWorld->IsShuttingDown())
        {
            sWorld->ShutdownCancel();
            ReplyEmbed(ix, "Shutdown Cancelled", "The pending shutdown/restart has been cancelled.", 0x00AE86);
            LogAudit(ix, "server.cancel", "", "ok");
        }
        else
            Reply(ix, "No shutdown or restart is currently pending.", true);
        return;
    }

    Reply(ix, "Unknown /server subcommand.", true);
}

void DiscordAdmin::HandlePlayerCommands(DiscordInteraction const& ix)
{
    if (auto sub = ix.FindSubcommand("info"))
    {
        std::string name = ix.GetOptionValue(*sub, "player");
        if (name.empty()) { Reply(ix, "Missing player name.", true); return; }
        Player* player = ObjectAccessor::FindPlayerByName(name, false);
        if (!player)
        {
            QueryResult result = CharacterDatabase.Query(
                "SELECT account, name, level, race, class, gender FROM characters WHERE name = '{}'", EscapeSql(name));
            if (!result)
            {
                Reply(ix, "Player '" + name + "' not found.", true);
                return;
            }
            Field* f = result->Fetch();
            std::string text = "**Name:** " + f[1].Get<std::string>() + " (offline)\n" +
                               "**Level:** " + std::to_string(f[2].Get<uint32>()) + "\n" +
                               "**Race:** " + DiscordChat::RaceKey(f[3].Get<uint8>()) + "\n" +
                               "**Class:** " + DiscordChat::ClassName(f[4].Get<uint8>());
            ReplyEmbed(ix, "Player Info", text, 0x5865F2);
            LogAudit(ix, "player.info", name, "ok (offline)");
            return;
        }
        std::string text = "**Name:** " + std::string(player->GetName()) + " (online)\n" +
                           "**Level:** " + std::to_string(player->GetLevel()) + "\n" +
                           "**Race:** " + DiscordChat::RaceKey(player->getRace(true)) + "\n" +
                           "**Class:** " + DiscordChat::ClassName(player->getClass()) + "\n" +
                           "**Guild:** " + player->GetGuildName() + "\n" +
                           "**Map:** " + std::to_string(player->GetMapId());
        ReplyEmbed(ix, "Player Info", text, 0x5865F2);
        LogAudit(ix, "player.info", name, "ok (online)");
        return;
    }
    if (auto sub = ix.FindSubcommand("kick"))
    {
        std::string name = ix.GetOptionValue(*sub, "player");
        std::string reason = ix.GetOptionValue(*sub, "reason");
        if (name.empty()) { Reply(ix, "Missing player name.", true); return; }
        std::string cmd = ".kick " + name + (reason.empty() ? "" : " " + reason);
        sWorld->QueueCliCommand(new CliCommandHolder(nullptr, cmd.c_str(),
                                                     [](void*, std::string_view out) { LOG_INFO("modules.discord.admin", "console: {}", out); },
                                                     [](void*, bool) {}));
        ReplyEmbed(ix, "Kick", "Kicking " + name + (reason.empty() ? "" : " (" + reason + ")"), 0xED4245);
        LogAudit(ix, "player.kick", name, "ok");
        return;
    }
    if (auto sub = ix.FindSubcommand("mute"))
    {
        std::string name = ix.GetOptionValue(*sub, "player");
        std::string minutes = ix.GetOptionValue(*sub, "minutes");
        std::string reason = ix.GetOptionValue(*sub, "reason");
        if (name.empty() || minutes.empty()) { Reply(ix, "Missing player name or duration.", true); return; }
        std::string cmd = ".mute " + name + " " + minutes + "m" + (reason.empty() ? "" : " " + reason);
        sWorld->QueueCliCommand(new CliCommandHolder(nullptr, cmd.c_str(),
                                                     [](void*, std::string_view out) { LOG_INFO("modules.discord.admin", "console: {}", out); },
                                                     [](void*, bool) {}));
        ReplyEmbed(ix, "Mute", "Muted " + name + " for " + minutes + " minutes.", 0xED4245);
        LogAudit(ix, "player.mute", name, "ok");
        return;
    }
    if (auto sub = ix.FindSubcommand("unmute"))
    {
        std::string name = ix.GetOptionValue(*sub, "player");
        if (name.empty()) { Reply(ix, "Missing player name.", true); return; }
        sWorld->QueueCliCommand(new CliCommandHolder(nullptr, (".unmute " + name).c_str(),
                                                     [](void*, std::string_view out) { LOG_INFO("modules.discord.admin", "console: {}", out); },
                                                     [](void*, bool) {}));
        ReplyEmbed(ix, "Unmute", "Unmuted " + name + ".", 0x00AE86);
        LogAudit(ix, "player.unmute", name, "ok");
        return;
    }
    if (auto sub = ix.FindSubcommand("teleport"))
    {
        std::string name = ix.GetOptionValue(*sub, "player");
        std::string location = ix.GetOptionValue(*sub, "location");
        if (name.empty() || location.empty()) { Reply(ix, "Missing player or location.", true); return; }
        Player* player = ObjectAccessor::FindPlayerByName(name, false);
        if (!player) { Reply(ix, "Player '" + name + "' is not online.", true); return; }
        GameTele const* tele = sObjectMgr->GetGameTele(location, true);
        if (!tele) { Reply(ix, "Unknown teleport location '" + location + "'.", true); return; }
        player->TeleportTo(tele->mapId, tele->position_x, tele->position_y, tele->position_z, tele->orientation);
        ReplyEmbed(ix, "Teleport", "Teleported " + name + " to " + location + ".", 0x5865F2);
        LogAudit(ix, "player.teleport", name + " -> " + location, "ok");
        return;
    }
    if (auto sub = ix.FindSubcommand("summon"))
    {
        std::string name = ix.GetOptionValue(*sub, "player");
        std::string target = ix.GetOptionValue(*sub, "target");
        std::string location = ix.GetOptionValue(*sub, "location");
        if (name.empty()) { Reply(ix, "Missing player name.", true); return; }
        Player* player = ObjectAccessor::FindPlayerByName(name, false);
        if (!player) { Reply(ix, "Player '" + name + "' is not online.", true); return; }

        if (!location.empty())
        {
            GameTele const* tele = sObjectMgr->GetGameTele(location, true);
            if (!tele) { Reply(ix, "Unknown teleport location '" + location + "'.", true); return; }
            player->TeleportTo(tele->mapId, tele->position_x, tele->position_y, tele->position_z, tele->orientation);
            ReplyEmbed(ix, "Summon", "Summoned " + name + " to " + location + ".", 0x5865F2);
            LogAudit(ix, "player.summon", name + " -> " + location, "ok");
            return;
        }
        if (!target.empty())
        {
            Player* dst = ObjectAccessor::FindPlayerByName(target, false);
            if (!dst) { Reply(ix, "Target player '" + target + "' is not online.", true); return; }
            player->TeleportTo(dst->GetMapId(), dst->GetPositionX(), dst->GetPositionY(), dst->GetPositionZ(), dst->GetOrientation());
            ReplyEmbed(ix, "Summon", "Summoned " + name + " to " + target + ".", 0x5865F2);
            LogAudit(ix, "player.summon", name + " -> " + target, "ok");
            return;
        }
        Reply(ix, "Provide a destination player or location for summon.", true);
        return;
    }
    if (auto sub = ix.FindSubcommand("level"))
    {
        std::string name = ix.GetOptionValue(*sub, "player");
        std::string level = ix.GetOptionValue(*sub, "level");
        if (name.empty() || level.empty()) { Reply(ix, "Missing player or level.", true); return; }
        RequestConfirmation(ix, "player.level", name + "|" + level, "Set the level of " + name + " to " + level + ".");
        return;
    }
    if (auto sub = ix.FindSubcommand("money"))
    {
        std::string name = ix.GetOptionValue(*sub, "player");
        std::string amount = ix.GetOptionValue(*sub, "amount");
        if (name.empty() || amount.empty()) { Reply(ix, "Missing player or amount.", true); return; }
        RequestConfirmation(ix, "player.money", name + "|" + amount, "Set " + name + "'s money to " + amount + " copper.");
        return;
    }
    if (auto sub = ix.FindSubcommand("item"))
    {
        std::string name = ix.GetOptionValue(*sub, "player");
        std::string item = ix.GetOptionValue(*sub, "item");
        std::string count = ix.GetOptionValue(*sub, "count");
        if (count.empty()) count = "1";
        if (name.empty() || item.empty()) { Reply(ix, "Missing player or item.", true); return; }
        RequestConfirmation(ix, "player.item", name + "|" + item + "|" + count,
                            "Grant " + count + "x item " + item + " to " + name + ".");
        return;
    }

    Reply(ix, "Unknown /player subcommand.", true);
}

void DiscordAdmin::HandleAccountCommands(DiscordInteraction const& ix)
{
    if (auto sub = ix.FindSubcommand("info"))
    {
        std::string name = ix.GetOptionValue(*sub, "account");
        if (name.empty()) { Reply(ix, "Missing account or player name.", true); return; }

        // Resolve a player name to its account (characters DB), or treat the
        // argument directly as an account name (auth DB).
        uint32 accountId = 0;
        QueryResult charResult = CharacterDatabase.Query(
            "SELECT account FROM characters WHERE name = '{}'", EscapeSql(name));
        if (charResult)
            accountId = charResult->Fetch()[0].Get<uint32>();

        QueryResult result;
        if (accountId)
            result = LoginDatabase.Query("SELECT id, username FROM account WHERE id = {}", accountId);
        else
            result = LoginDatabase.Query("SELECT id, username FROM account WHERE username = '{}'", EscapeSql(name));

        if (!result)
        {
            Reply(ix, "Account or player '" + name + "' not found.", true);
            return;
        }
        Field* f = result->Fetch();
        std::string info = "**Account ID:** " + std::to_string(f[0].Get<uint32>()) +
                           "\n**Username:** " + f[1].Get<std::string>();
        if (accountId)
            info += "\n**Player:** " + name;
        ReplyEmbed(ix, "Account Info", info, 0x5865F2);
        LogAudit(ix, "account.info", name, "ok");
        return;
    }
    if (auto sub = ix.FindSubcommand("ban"))
    {
        std::string name = ix.GetOptionValue(*sub, "account");
        std::string days = ix.GetOptionValue(*sub, "days");
        std::string reason = ix.GetOptionValue(*sub, "reason");
        if (name.empty() || days.empty()) { Reply(ix, "Missing account or duration.", true); return; }
        RequestConfirmation(ix, "account.ban", name + "|" + days + "|" + (reason.empty() ? "Discord admin action" : reason),
                            "Ban account '" + name + "' for " + days + " days.");
        return;
    }
    if (auto sub = ix.FindSubcommand("unban"))
    {
        std::string name = ix.GetOptionValue(*sub, "account");
        if (name.empty()) { Reply(ix, "Missing account name.", true); return; }
        RequestConfirmation(ix, "account.unban", name + "||", "Unban account '" + name + "'.");
        return;
    }
    if (auto sub = ix.FindSubcommand("mute"))
    {
        std::string name = ix.GetOptionValue(*sub, "account");
        std::string minutes = ix.GetOptionValue(*sub, "minutes");
        if (name.empty() || minutes.empty()) { Reply(ix, "Missing account or duration.", true); return; }
        sWorld->QueueCliCommand(new CliCommandHolder(nullptr, (".mute " + name + " " + minutes + "m").c_str(),
                                                     [](void*, std::string_view out) { LOG_INFO("modules.discord.admin", "console: {}", out); },
                                                     [](void*, bool) {}));
        ReplyEmbed(ix, "Mute", "Muted account '" + name + "' for " + minutes + " minutes.", 0xED4245);
        LogAudit(ix, "account.mute", name, "ok");
        return;
    }
    if (auto sub = ix.FindSubcommand("unmute"))
    {
        std::string name = ix.GetOptionValue(*sub, "account");
        if (name.empty()) { Reply(ix, "Missing account name.", true); return; }
        sWorld->QueueCliCommand(new CliCommandHolder(nullptr, (".unmute " + name).c_str(),
                                                     [](void*, std::string_view out) { LOG_INFO("modules.discord.admin", "console: {}", out); },
                                                     [](void*, bool) {}));
        ReplyEmbed(ix, "Unmute", "Unmuted account '" + name + "'.", 0x00AE86);
        LogAudit(ix, "account.unmute", name, "ok");
        return;
    }

    Reply(ix, "Unknown /account subcommand.", true);
}

void DiscordAdmin::HandleAnnounceCommands(DiscordInteraction const& ix)
{
    if (auto sub = ix.FindSubcommand("global"))
    {
        std::string msg = ix.GetOptionValue(*sub, "message");
        if (msg.empty()) { Reply(ix, "Missing message.", true); return; }
        sWorld->QueueCliCommand(new CliCommandHolder(nullptr, (".announce " + msg).c_str(),
                                                     [](void*, std::string_view out) { LOG_INFO("modules.discord.admin", "console: {}", out); },
                                                     [](void*, bool) {}));
        ReplyEmbed(ix, "Announcement", "Global announcement sent.", 0x00AE86);
        LogAudit(ix, "announce.global", msg, "ok");
        return;
    }
    if (auto sub = ix.FindSubcommand("notification"))
    {
        std::string msg = ix.GetOptionValue(*sub, "message");
        if (msg.empty()) { Reply(ix, "Missing message.", true); return; }
        sWorld->QueueCliCommand(new CliCommandHolder(nullptr, (".notification " + msg).c_str(),
                                                     [](void*, std::string_view out) { LOG_INFO("modules.discord.admin", "console: {}", out); },
                                                     [](void*, bool) {}));
        ReplyEmbed(ix, "Notification", "Notification sent.", 0x00AE86);
        LogAudit(ix, "announce.notification", msg, "ok");
        return;
    }
    if (auto sub = ix.FindSubcommand("server"))
    {
        std::string msg = ix.GetOptionValue(*sub, "message");
        if (msg.empty()) { Reply(ix, "Missing message.", true); return; }
        sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, msg, nullptr);
        ReplyEmbed(ix, "Server Message", "Server-style message sent.", 0x00AE86);
        LogAudit(ix, "announce.server", msg, "ok");
        return;
    }
    Reply(ix, "Unknown /announce subcommand.", true);
}

void DiscordAdmin::HandleConfigCommands(DiscordInteraction const& ix)
{
    if (auto sub = ix.FindSubcommand("reload"))
    {
        // Reload the worldserver config (which fires OnAfterConfigLoad(true) and
        // in turn reloads the Discord module config + refreshes emojis/presence).
        sWorld->LoadConfigSettings(true);
        ReplyEmbed(ix, "Config Reloaded", "Discord module configuration reloaded.", 0x00AE86);
        LogAudit(ix, "config.reload", "", "ok");
        return;
    }
    Reply(ix, "Unknown /config subcommand.", true);
}

void DiscordAdmin::HandleDiscordCommands(DiscordInteraction const& ix)
{
    // /discord emojis reload
    auto emojis = ix.FindSubcommand("emojis");
    if (emojis && emojis->FindSubcommand("reload"))
    {
        sDiscordMgr->Emojis()->Refresh();
        ReplyEmbed(ix, "Emojis", "Emoji cache reload requested.", 0x00AE86);
        LogAudit(ix, "discord.emojis.reload", "", "ok");
        return;
    }
    Reply(ix, "Unknown /discord subcommand.", true);
}

void DiscordAdmin::HandleConsoleCommand(DiscordInteraction const& ix)
{
    if (!sDiscordMgr->Config()->AllowRawConsole)
    {
        Reply(ix, "Raw console is disabled on this server.", true);
        return;
    }
    std::string cmd = ix.GetTopLevelValue("command");
    if (cmd.empty()) { Reply(ix, "Missing command.", true); return; }
    RequestConfirmation(ix, "console", cmd, "Execute raw worldserver command: `" + cmd + "`");
}

// ---------------------------------------------------------------------------
// /setup
// ---------------------------------------------------------------------------

namespace
{
    struct RoleTemplate
    {
        char const* name;
        uint32_t color;
    };

    RoleTemplate const kRoleTemplates[] = {
        // Factions
        {"Alliance", 0x1E90FF}, {"Horde", 0xC41E3A},
        // Races
        {"Human", 0xF0E68C}, {"Orc", 0x8B8B00}, {"Dwarf", 0xB87333}, {"NightElf", 0x6A5ACD},
        {"Undead", 0x9E9E9E}, {"Tauren", 0xA0522D}, {"Gnome", 0xFFF468}, {"Troll", 0x2E8B57},
        {"BloodElf", 0xEE82EE}, {"Draenei", 0x87CEEB},
        // Classes (WoW class colors)
        {"Paladin", 0xF58CBA}, {"Warrior", 0xC79C6E}, {"Hunter", 0xAAD372}, {"Rogue", 0xFFF468},
        {"Priest", 0xFFFFFF}, {"Death Knight", 0xC41E3A}, {"Shaman", 0x0070DD}, {"Mage", 0x3FC7EB},
        {"Warlock", 0x8788EE}, {"Druid", 0xFF7C0A},
        // Staff
        {"Admin", 0xED4245}, {"Moderator", 0x00AE86}
    };

    struct RoleSetupState
    {
        uint64_t applicationId = 0;
        std::string token;
        uint32_t accountId = 0;   // for the in-game reply target
        std::deque<std::pair<std::string, uint32_t>> pendingRoles;
        std::vector<std::pair<std::string, uint64_t>> created;
        std::vector<std::string> existing;
        std::vector<std::string> failed;
    };

    std::string BuildRoleSummary(RoleSetupState const& state)
    {
        std::string out = "**Role Setup Complete**";
        if (!state.created.empty())
        {
            out += "\n\n**Created:**";
            for (auto const& [name, id] : state.created)
                out += "\n- " + name + " (`" + std::to_string(id) + "`)";
        }
        if (!state.existing.empty())
        {
            out += "\n\n**Already existed:**";
            for (std::string const& name : state.existing)
                out += "\n- " + name;
        }
        if (!state.failed.empty())
        {
            out += "\n\n**Failed** (check the bot has the Manage Roles permission):";
            for (std::string const& name : state.failed)
                out += "\n- " + name;
        }
        if (state.created.empty() && state.existing.empty() && state.failed.empty())
            out += "\n\nNothing to create.";

        out += "\n\nPut the IDs into `Discord.Roles.Tags` for the class/race/faction roles, "
               "`Discord.Roles.AdminIds` + `Discord.Admin.AllowedRoleIds` for staff, then reload.";
        return out;
    }

    // Creates the next pending role, then schedules the following one ~1s later.
    // Runs on the game thread. Creating one role at a time avoids Discord's
    // guild role-creation rate limit (10 roles / 10s) and guarantees the summary
    // always appears when the queue is empty.
    void CreateNextRole(std::shared_ptr<RoleSetupState> state, std::function<void()> onDone)
    {
        if (state->pendingRoles.empty())
        {
            if (onDone)
                onDone();
            return;
        }

        auto role = state->pendingRoles.front();
        state->pendingRoles.pop_front();
        std::string name = role.first;
        uint32_t color = role.second;

        auto client = sDiscordMgr->Client();
        if (!client)
        {
            state->failed.push_back(name);
            sDiscordMgr->PostToGameThreadDelayed(1000, [state, onDone]() { CreateNextRole(state, onDone); });
            return;
        }

        client->CreateGuildRole(name, color, [state, name, onDone](std::string const& json) {
            sDiscordMgr->PostToGameThread([state, name, onDone, json]() {
                DiscordJson::Value parsed;
                DiscordJson::Parse(json, parsed);
                long long code = DiscordJson::GetInt(parsed, "code");
                if (DiscordJson::GetInt(parsed, "id") != 0)
                {
                    uint64_t roleId = uint64_t(DiscordJson::GetInt(parsed, "id"));
                    state->created.emplace_back(name, roleId);

                    // Keep Admin above Moderator, both above the other roles.
                    if (name == "Admin" || name == "Moderator")
                    {
                        if (auto client = sDiscordMgr->Client())
                            client->ModifyGuildRolePosition(roleId, name == "Admin" ? 1000 : 999);
                    }
                }
                else if (code == 50013) // Missing Permissions
                    state->failed.push_back(name);
                else // 400 -> name already taken
                    state->existing.push_back(name);

                sDiscordMgr->PostToGameThreadDelayed(1000, [state, onDone]() { CreateNextRole(state, onDone); });
            });
        });
    }

    // Fills the queue with every template role not already present in the guild
    // and starts the sequential creation. onDone fires on the game thread when
    // all roles have been processed.
    void FireRoleCreations(std::shared_ptr<RoleSetupState> state, std::function<void()> onDone)
    {
        for (RoleTemplate const& t : kRoleTemplates)
        {
            bool exists = false;
            for (auto const& [id, info] : sDiscordMgr->GetRoleCache())
            {
                (void)id;
                if (info.name == t.name)
                {
                    exists = true;
                    break;
                }
            }
            if (exists)
                state->existing.push_back(t.name);
            else
                state->pendingRoles.emplace_back(t.name, t.color);
        }
        CreateNextRole(state, onDone);
    }
}

void DiscordAdmin::HandleSetupCommands(DiscordInteraction const& ix)
{
    if (auto sub = ix.FindSubcommand("roles"))
    {
        CreateRoles(ix);
        return;
    }
    Reply(ix, "Unknown /setup subcommand.", true);
}

void DiscordAdmin::CreateRoles(DiscordInteraction const& ix)
{
    auto client = sDiscordMgr->Client();
    if (!client)
        return;

    // Deferred response: role creation happens over the network after this.
    DiscordJson::Writer w;
    w.Open();
    w.Field("type", 5LL);
    w.Close();
    PostInteraction(ix, w.out);

    auto state = std::make_shared<RoleSetupState>();
    state->applicationId = ix.applicationId;
    state->token = ix.token;

    FireRoleCreations(state, [state]() {
        auto* client = sDiscordMgr->Client();
        if (!client)
            return;
        DiscordJson::Writer w;
        w.Open();
        w.Field("content", BuildRoleSummary(*state));
        w.Close();
        client->CreateInteractionFollowup(state->applicationId, state->token, w.out);
    });
}

void DiscordAdmin::CreateRolesInGame(ChatHandler* handler)
{
    if (!handler->GetSession())
    {
        handler->SendSysMessage("This command can only be used in-game.");
        return;
    }
    if (!sDiscordMgr->Client())
    {
        handler->SendSysMessage("Discord is not connected.");
        return;
    }

    uint32_t accountId = handler->GetSession()->GetAccountId();
    handler->SendSysMessage("Creating Discord roles... (this may take a moment)");

    auto state = std::make_shared<RoleSetupState>();
    state->accountId = accountId;

    FireRoleCreations(state, [state]() {
        std::string summary = BuildRoleSummary(*state);
        if (WorldSession* session = sWorldSessionMgr->FindSession(state->accountId))
            ChatHandler(session).SendSysMessage(summary);
    });
}

// ---------------------------------------------------------------------------
// Audit logging
// ---------------------------------------------------------------------------

void DiscordAdmin::LogAudit(DiscordInteraction const& ix, std::string const& command,
                            std::string const& args, std::string const& result)
{
    CharacterDatabase.Execute(
        "INSERT INTO mod_discord_audit_log (discord_username, discord_id, account_id, command, arguments, result, realm_id) "
        "VALUES ('{}', '{}', 0, '{}', '{}', '{}', {})",
        EscapeSql(ix.userTag), EscapeSql(std::to_string(ix.userId)),
        EscapeSql(command), EscapeSql(args), EscapeSql(result), sDiscordMgr->Config()->GameRealmId);
}