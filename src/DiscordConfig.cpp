#include "DiscordConfig.h"
#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace
{
    std::vector<uint64_t> ParseIdList(std::string const& raw)
    {
        std::vector<uint64_t> out;
        size_t start = 0;
        while (start <= raw.size())
        {
            size_t comma = raw.find(',', start);
            std::string part = comma == std::string::npos ? raw.substr(start) : raw.substr(start, comma - start);
            size_t b = part.find_first_not_of(" \t\r\n");
            size_t e = part.find_last_not_of(" \t\r\n");
            if (b != std::string::npos)
            {
                part = part.substr(b, e - b + 1);
                try { out.push_back(std::stoull(part)); }
                catch (...) { LOG_WARN("modules.discord.config", "Discord config: ignoring non-numeric id '{}'", part); }
            }
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return out;
    }

    std::string TrimString(std::string s)
    {
        auto const notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    // Parse a hex color ("5865F2" or "#5865F2") into 0xRRGGBB.
    uint32 ParseHexColor(std::string const& raw, uint32 def)
    {
        std::string s = TrimString(raw);
        if (!s.empty() && s[0] == '#')
            s = s.substr(1);
        if (s.size() != 6)
            return def;
        uint32 value = 0;
        for (char c : s)
        {
            value <<= 4;
            if (c >= '0' && c <= '9') value |= c - '0';
            else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
            else return def;
        }
        return value;
    }

    // Split a comma-separated list and trim each entry.
    std::vector<std::string> TokenizeRoleColors(std::string const& raw)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= raw.size())
        {
            size_t comma = raw.find(',', start);
            std::string part = comma == std::string::npos ? raw.substr(start) : raw.substr(start, comma - start);
            size_t b = part.find_first_not_of(" \t\r\n");
            size_t e = part.find_last_not_of(" \t\r\n");
            if (b != std::string::npos)
                out.push_back(part.substr(b, e - b + 1));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return out;
    }

    uint32 GetOptionUInt(std::string const& name, uint32 def)
    {
        return sConfigMgr->GetOption<uint32>(name, def);
    }

    // Discord snowflake IDs exceed the uint32 range; read them as uint64.
    uint64_t GetOptionUInt64(std::string const& name, uint64_t def)
    {
        return sConfigMgr->GetOption<uint64_t>(name, def);
    }

    bool GetOptionBool(std::string const& name, bool def)
    {
        return sConfigMgr->GetOption<bool>(name, def);
    }

    std::string GetOptionStr(std::string const& name, std::string const& def)
    {
        return sConfigMgr->GetOption<std::string>(name, def);
    }
}

bool DiscordConfig::IsChannelAllowedForChat(uint64_t channelId) const
{
    if (SplitFactions)
    {
        if (channelId == AllianceChannelId || channelId == HordeChannelId)
            return true;
    }
    else if (channelId == GlobalChannelId)
    {
        return true;
    }
    for (uint64_t extra : AllowChannels)
        if (channelId == extra)
            return true;
    return false;
}

bool DiscordConfig::IsAdminAuthorized(uint64_t userId, std::vector<uint64_t> const& roles) const
{
    if (AdminUserIds.find(userId) != AdminUserIds.end())
        return true;
    for (uint64_t role : roles)
        if (AdminRoleIds.find(role) != AdminRoleIds.end())
            return true;
    return false;
}

void DiscordConfig::Load()
{
    Enable = GetOptionBool("Discord.Enable", true);
    GameRealmId = GetOptionUInt("Discord.GameRealmId", 1);

    Token = GetOptionStr("Discord.Bot.Token", "");
    ApplicationId = GetOptionUInt64("Discord.Bot.ApplicationId", 0);
    GuildId = GetOptionUInt64("Discord.Bot.GuildId", 0);
    GuildMembersIntent = GetOptionBool("Discord.Bot.GuildMembersIntent", true);
    MessageContentIntent = GetOptionBool("Discord.Bot.MessageContentIntent", true);
    // GUILDS + GUILD_MESSAGES are always requested. The privileged intents
    // (GUILD_MEMBERS, MESSAGE_CONTENT) only if enabled in config; the owner must
    // also toggle them in the Discord Developer Portal or Discord will reject the
    // connection with a "disallowed intent" error.
    Intents = (1u << 0) | (1u << 9); // GUILDS, GUILD_MESSAGES
    if (GuildMembersIntent)
        Intents |= (1u << 1);        // GUILD_MEMBERS
    if (MessageContentIntent)
        Intents |= (1u << 15);       // MESSAGE_CONTENT
    Intents |= GetOptionUInt("Discord.Bot.Intents", 0);
    ReconnectDelay = std::max<uint32>(1, GetOptionUInt("Discord.Bot.ReconnectDelay", 5));

    ServerName = GetOptionStr("Discord.Branding.ServerName", "AzerothCore");
    ServerDescription = GetOptionStr("Discord.Branding.ServerDescription", "");
    ServerIconUrl = GetOptionStr("Discord.Branding.ServerIconUrl", "");
    WebsiteUrl = GetOptionStr("Discord.Branding.WebsiteUrl", "");

    GlobalChatEnable = GetOptionBool("Discord.GlobalChat.Enable", true);
    DiscordTag = GetOptionStr("Discord.GlobalChat.DiscordTag", "[Discord]");
    DiscordTagColor = GetOptionStr("Discord.GlobalChat.TagColor", "5865F2");
    AllowChannels = ParseIdList(GetOptionStr("Discord.GlobalChat.AllowChannels", ""));
    UseWebhooks = GetOptionBool("Discord.GlobalChat.UseWebhooks", true);
    HideWebhookName = GetOptionBool("Discord.GlobalChat.HideWebhookName", true);
    GlobalChatIncludeBots = GetOptionBool("Discord.GlobalChat.IncludeBots", false);
    Attachments = GetOptionBool("Discord.GlobalChat.Attachments", false);
    Provider = GetOptionStr("Discord.GlobalChat.Provider", "AUTO");
    RelayDirectGameMessages = GetOptionBool("Discord.GlobalChat.RelayDirectGameMessages", true);

    // Discord role display settings.
    RolesEnabled = GetOptionBool("Discord.Roles.Enable", true);
    UseLiveRoleColors = GetOptionBool("Discord.Roles.UseLiveColors", true);
    AdminColor = ParseHexColor(GetOptionStr("Discord.Roles.AdminColor", "ed4245"), 0xED4245);
    DisplayAdminRoleIds.clear();
    for (uint64_t id : ParseIdList(GetOptionStr("Discord.Roles.AdminIds", "")))
        DisplayAdminRoleIds.insert(id);
    RoleColorOverrides.clear();
    for (std::string const& pair : TokenizeRoleColors(GetOptionStr("Discord.Roles.Colors", "")))
    {
        size_t colon = pair.find(':');
        if (colon == std::string::npos)
            continue;
        try
        {
            uint64_t id = std::stoull(pair.substr(0, colon));
            uint32 color = ParseHexColor(pair.substr(colon + 1), 0);
            if (id && color)
                RoleColorOverrides[id] = color;
        }
        catch (...) { LOG_WARN("modules.discord.config", "Discord config: ignoring malformed role color '{}'", pair); }
    }

    // Ordered role tags shown before the display name, e.g. "roleid:Paladin,roleid:Orc".
    RoleTags.clear();
    for (std::string const& entry : TokenizeRoleColors(GetOptionStr("Discord.Roles.Tags", "")))
    {
        size_t colon = entry.find(':');
        try
        {
            uint64_t id = std::stoull(colon == std::string::npos ? entry : entry.substr(0, colon));
            std::string label = colon == std::string::npos ? "" : entry.substr(colon + 1);
            if (id)
                RoleTags.emplace_back(id, label);
        }
        catch (...) { LOG_WARN("modules.discord.config", "Discord config: ignoring malformed role tag '{}'", entry); }
    }

    SplitFactions = GetOptionBool("Discord.GlobalChat.SplitFactions", false);
    GlobalChannelId = GetOptionUInt64("Discord.GlobalChat.ChannelId", 0);
    AllianceChannelId = GetOptionUInt64("Discord.GlobalChat.AllianceChannelId", 0);
    HordeChannelId = GetOptionUInt64("Discord.GlobalChat.HordeChannelId", 0);

    WebhookGlobalChat = GetOptionStr("Discord.Webhook.GlobalChat", "");
    WebhookAllianceChat = GetOptionStr("Discord.Webhook.AllianceChat", "");
    WebhookHordeChat = GetOptionStr("Discord.Webhook.HordeChat", "");
    WebhookEvents = GetOptionStr("Discord.Webhook.Events", "");
    WebhookAdmin = GetOptionStr("Discord.Webhook.Admin", "");

    EmojisEnable = GetOptionBool("Discord.Emojis.Enable", true);
    EmojisResolveByName = GetOptionBool("Discord.Emojis.ResolveByName", true);
    EmojisShowFaction = GetOptionBool("Discord.Emojis.ShowFaction", true);
    EmojisShowRace = GetOptionBool("Discord.Emojis.ShowRace", true);
    EmojisShowClass = GetOptionBool("Discord.Emojis.ShowClass", true);
    EmojisFactionEnable = GetOptionBool("Discord.Emojis.Faction.Enable", true);
    EmojisClassEnable = GetOptionBool("Discord.Emojis.Class.Enable", true);
    EmojisRaceEnable = GetOptionBool("Discord.Emojis.Race.Enable", true);
    EmojiFactionAlliance = TrimString(GetOptionStr("Discord.Emojis.Faction.Alliance", ":factionalliance:"));
    EmojiFactionHorde = TrimString(GetOptionStr("Discord.Emojis.Faction.Horde", ":factionhorde:"));
    EmojiGm = TrimString(GetOptionStr("Discord.Emojis.Gm", ":gmbadge:"));

    // Class emojis by class id.
    EmojiClass.clear();
    EmojiClass[1] = TrimString(GetOptionStr("Discord.Emojis.Class.Warrior", ":warriorwarrior:"));
    EmojiClass[2] = TrimString(GetOptionStr("Discord.Emojis.Class.Paladin", ":classpaladin:"));
    EmojiClass[3] = TrimString(GetOptionStr("Discord.Emojis.Class.Hunter", ":classhunter:"));
    EmojiClass[4] = TrimString(GetOptionStr("Discord.Emojis.Class.Rogue", ":classrogue:"));
    EmojiClass[5] = TrimString(GetOptionStr("Discord.Emojis.Class.Priest", ":classpriest:"));
    EmojiClass[6] = TrimString(GetOptionStr("Discord.Emojis.Class.DeathKnight", ":classdk:"));
    EmojiClass[7] = TrimString(GetOptionStr("Discord.Emojis.Class.Shaman", ":classshaman:"));
    EmojiClass[8] = TrimString(GetOptionStr("Discord.Emojis.Class.Mage", ":classmage:"));
    EmojiClass[9] = TrimString(GetOptionStr("Discord.Emojis.Class.Warlock", ":classwarlock:"));
    EmojiClass[10] = TrimString(GetOptionStr("Discord.Emojis.Class.Druid", ":classdruid:"));
    // Extensible: optional custom class entries for future class IDs. Demon
    // Hunter (class id 11) does not exist in WotLK but can still be configured.
    std::string demonHunter = TrimString(GetOptionStr("Discord.Emojis.Class.DemonHunter", ":classdh:"));
    if (!demonHunter.empty())
        EmojiClass[11] = demonHunter;

    // Race emojis (race id -> {male, female}).
    EmojiRace.clear();
    auto race = [this](uint32 id, char const* base)
    {
        std::string key = base;
        std::string lower = key;
        for (char& c : lower)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        EmojiRace[id] = {
            TrimString(GetOptionStr("Discord.Emojis.Race." + key + ".Male", ":race" + lower + "ma:")),
            TrimString(GetOptionStr("Discord.Emojis.Race." + key + ".Female", ":race" + lower + "fe:"))
        };
    };
    race(1, "Human");
    race(2, "Orc");
    race(3, "Dwarf");
    race(4, "NightElf");
    race(5, "Undead");
    race(6, "Tauren");
    race(7, "Gnome");
    race(8, "Troll");
    race(9, "BloodElf");
    race(10, "Draenei");

    AdminEnable = GetOptionBool("Discord.Admin.Enable", true);
    AdminChannelId = GetOptionUInt64("Discord.Admin.ChannelId", 0);
    ConfirmTimeout = std::max<uint32>(10, GetOptionUInt("Discord.Admin.ConfirmTimeout", 60));
    AllowRawConsole = GetOptionBool("Discord.Admin.AllowRawConsole", false);
    AdminRoleIds.clear();
    for (uint64_t id : ParseIdList(GetOptionStr("Discord.Admin.AllowedRoleIds", "")))
        AdminRoleIds.insert(id);
    AdminUserIds.clear();
    for (uint64_t id : ParseIdList(GetOptionStr("Discord.Admin.AllowedUserIds", "")))
        AdminUserIds.insert(id);
    CmdServer = GetOptionBool("Discord.Admin.Commands.Server", true);
    CmdPlayer = GetOptionBool("Discord.Admin.Commands.Player", true);
    CmdAccount = GetOptionBool("Discord.Admin.Commands.Account", true);
    CmdAnnounce = GetOptionBool("Discord.Admin.Commands.Announce", true);
    CmdConfig = GetOptionBool("Discord.Admin.Commands.Config", true);
    CmdConsole = GetOptionBool("Discord.Admin.Commands.Console", false);
    SelfConfirmOnly = GetOptionBool("Discord.Admin.SelfConfirmOnly", true);
    RequireConfirm = GetOptionBool("Discord.Confirm.RequireConfirm", true);

    EventsEnable = GetOptionBool("Discord.Events.Enable", true);
    EventsChannelId = GetOptionUInt64("Discord.Events.ChannelId", 0);
    EventPlayerLogin = GetOptionBool("Discord.Events.PlayerLogin", true);
    EventPlayerLogout = GetOptionBool("Discord.Events.PlayerLogout", true);
    EventMemberJoin = GetOptionBool("Discord.Events.MemberJoin", true);
    EventMemberLeave = GetOptionBool("Discord.Events.MemberLeave", true);
    EventAchievement = GetOptionBool("Discord.Events.Achievement", true);
    EventBossKill = GetOptionBool("Discord.Events.BossKill", true);
    EventBan = GetOptionBool("Discord.Events.Ban", true);
    EventMute = GetOptionBool("Discord.Events.Mute", true);
    EventGmCommand = GetOptionBool("Discord.Events.GmCommand", true);
    EventServerStartup = GetOptionBool("Discord.Events.ServerStartup", true);
    EventServerShutdown = GetOptionBool("Discord.Events.ServerShutdown", true);
    EventServerRestart = GetOptionBool("Discord.Events.ServerRestart", true);
    EventServerError = GetOptionBool("Discord.Events.ServerError", true);
    AchievementMinValue = GetOptionUInt("Discord.Events.Achievement.MinValue", 0);
    BossDungeon = GetOptionBool("Discord.Events.BossKill.Dungeon", true);
    BossRaid = GetOptionBool("Discord.Events.BossKill.Raid", true);
    BossWorld = GetOptionBool("Discord.Events.BossKill.World", true);
    BossFinalBossOnly = GetOptionBool("Discord.Events.BossKill.FinalBossOnly", false);

    ShowPopulation = GetOptionBool("Discord.Playerbots.ShowPopulation", true);
    IncludeBotsInChat = GetOptionBool("Discord.Playerbots.IncludeInChat", false);
    IncludeBotsInEvents = GetOptionBool("Discord.Playerbots.IncludeInEvents", false);

    FilterEnable = GetOptionBool("Discord.Filter.Enable", false);
    FilterDiscordToGame = GetOptionBool("Discord.Filter.DiscordToGame", true);
    FilterGameToDiscord = GetOptionBool("Discord.Filter.GameToDiscord", true);
    FilterMode = GetOptionStr("Discord.Filter.Mode", "block");
    FilterWords = GetOptionStr("Discord.Filter.Words", "");

    StatusEnable = GetOptionBool("Discord.ServerStatus.Enable", false);
    StatusChannelId = GetOptionUInt64("Discord.ServerStatus.ChannelId", 0);
    StatusUpdateInterval = std::max<uint32>(10, GetOptionUInt("Discord.ServerStatus.UpdateInterval", 60));
    StatusMessageId = GetOptionUInt64("Discord.ServerStatus.MessageId", 0);

    PresenceEnable = GetOptionBool("Discord.Presence.Enable", true);
    PresenceUpdateInterval = std::max<uint32>(10, GetOptionUInt("Discord.Presence.UpdateInterval", 60));
    PresenceActivityType = GetOptionStr("Discord.Presence.ActivityType", "Playing");
    PresenceText = GetOptionStr("Discord.Presence.Text", "Playing {realm} | {players} players");

    DebugLogHttp = GetOptionBool("Discord.Debug.LogHttp", false);
    DebugLogGateway = GetOptionBool("Discord.Debug.LogGateway", false);
}