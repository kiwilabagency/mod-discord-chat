#ifndef DISCORD_CONFIG_H_
#define DISCORD_CONFIG_H_

#include "DiscordModule.h"
#include <map>
#include <set>
#include <vector>

// Mirrors the discord-integration.conf settings into typed C++ fields.
class DiscordConfig
{
public:
    void Load();

    // ---- General ----
    bool Enable = false;
    uint32 GameRealmId = 1;

    // ---- Bot ----
    std::string Token;
    uint64_t ApplicationId = 0;
    uint64_t GuildId = 0;
    uint32 Intents = 0;
    bool GuildMembersIntent = true;
    bool MessageContentIntent = true;
    uint32 ReconnectDelay = 5;

    // ---- Branding ----
    std::string ServerName = "AzerothCore";
    std::string ServerDescription;
    std::string ServerIconUrl;
    std::string WebsiteUrl;

    // ---- Global chat ----
    bool GlobalChatEnable = true;
    std::string DiscordTag = "[Discord]";
    std::string DiscordTagColor = "5865F2";       // hex RRGGBB of the [Discord] tag
    std::vector<uint64_t> AllowChannels;
    bool UseWebhooks = true;
    bool HideWebhookName = true;              // use an invisible webhook username (compact messages)
    bool GlobalChatIncludeBots = false;
    bool Attachments = false;
    std::string Provider = "AUTO";
    bool RelayDirectGameMessages = true;

    // ---- Discord roles (in-game name coloring) ----
    bool RolesEnabled = true;
    bool UseLiveRoleColors = true;                // fetch real guild role colors
    uint32 AdminColor = 0xED4245;                 // forced color for admin roles
    std::set<uint64_t> DisplayAdminRoleIds;       // roles shown as admins (Blizz icon + red)
    std::map<uint64_t, uint32> RoleColorOverrides; // roleId -> RGB override
    std::vector<std::pair<uint64_t, std::string>> RoleTags; // ordered roleId -> "[label]" prefix

    // ---- Faction channels ----
    bool SplitFactions = false;
    uint64_t GlobalChannelId = 0;
    uint64_t AllianceChannelId = 0;
    uint64_t HordeChannelId = 0;

    // ---- Webhooks ----
    std::string WebhookGlobalChat;
    std::string WebhookAllianceChat;
    std::string WebhookHordeChat;
    std::string WebhookEvents;
    std::string WebhookAdmin;

    // ---- Emojis ----
    bool EmojisEnable = true;
    bool EmojisResolveByName = true;
    bool EmojisShowFaction = true;
    bool EmojisShowRace = true;
    bool EmojisShowClass = true;
    bool EmojisFactionEnable = true;
    bool EmojisClassEnable = true;
    bool EmojisRaceEnable = true;
    std::string EmojiFactionAlliance = ":factionalliance:";
    std::string EmojiFactionHorde = ":factionhorde:";
    std::string EmojiGm = ":gmbadge:";              // Blizz/GM badge emoji (game->discord)
    std::map<uint8, std::string> EmojiClass;               // class id -> name
    std::map<uint8, std::pair<std::string, std::string>> EmojiRace; // race id -> (male, female)

    // ---- Admin ----
    bool AdminEnable = true;
    uint64_t AdminChannelId = 0;
    uint32 ConfirmTimeout = 60;
    bool AllowRawConsole = false;
    std::set<uint64_t> AdminRoleIds;
    std::set<uint64_t> AdminUserIds;
    bool CmdServer = true;
    bool CmdPlayer = true;
    bool CmdAccount = true;
    bool CmdAnnounce = true;
    bool CmdConfig = true;
    bool CmdConsole = false;
    bool SelfConfirmOnly = true;
    bool RequireConfirm = true;

    // ---- Events ----
    bool EventsEnable = true;
    uint64_t EventsChannelId = 0;
    bool EventPlayerLogin = true;
    bool EventPlayerLogout = true;
    bool EventMemberJoin = true;
    bool EventMemberLeave = true;
    bool EventAchievement = true;
    bool EventBossKill = true;
    bool EventBan = true;
    bool EventMute = true;
    bool EventGmCommand = true;
    bool EventServerStartup = true;
    bool EventServerShutdown = true;
    bool EventServerRestart = true;
    bool EventServerError = true;
    uint32 AchievementMinValue = 0;
    bool BossDungeon = true;
    bool BossRaid = true;
    bool BossWorld = true;
    bool BossFinalBossOnly = false;

    // ---- Playerbots ----
    bool ShowPopulation = true;
    bool IncludeBotsInChat = false;
    bool IncludeBotsInEvents = false;

    // ---- Chat filter ----
    bool FilterEnable = false;
    bool FilterDiscordToGame = true;   // apply filter to Discord -> game chat
    bool FilterGameToDiscord = true;   // apply filter to game -> Discord chat
    std::string FilterMode = "block";  // "block" (drop message) or "censor" (***)
    std::string FilterWords;           // comma separated banned words ("" = starter list)

    // ---- Server status ----
    bool StatusEnable = false;
    uint64_t StatusChannelId = 0;
    uint32 StatusUpdateInterval = 60;
    uint64_t StatusMessageId = 0;

    // ---- Presence ----
    bool PresenceEnable = true;
    uint32 PresenceUpdateInterval = 60;
    std::string PresenceActivityType = "Playing";
    std::string PresenceText = "Playing {realm} | {players} players";

    // ---- Debug ----
    bool DebugLogHttp = false;
    bool DebugLogGateway = false;

    // Helpers ---------------------------------------------------------------
    bool IsChannelAllowedForChat(uint64_t channelId) const;
    bool IsAdminAuthorized(uint64_t userId, std::vector<uint64_t> const& roles) const;
};

#endif // DISCORD_CONFIG_H_