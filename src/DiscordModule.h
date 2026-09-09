/*
 * This file is part of the mod-discord-chat module for AzerothCore.
 *
 * Native Discord integration for AzerothCore 3.3.5a.
 *
 * Architecture overview
 * ---------------------
 * All Discord networking (gateway websocket + REST HTTPS) runs on a dedicated
 * worker thread owned by the DiscordClient. It never runs on the worldserver
 * gameplay thread, so worldserver game loop latency is never affected by
 * Discord traffic. The game thread communicates with the client thread through
 * lock-protected outbound queues; inbound events are marshalled back onto the
 * game thread via the worldserver tick (DiscordMgr::Update).
 *
 * Threading summary:
 *   - DiscordMgr            : game-thread singleton; owns config, emojis, chat
 *                             bridge, account links, admin, events, presence.
 *   - DiscordClient         : worker-thread networking + gateway + REST queue.
 *   - DiscordQueue          : lock-guarded outbound message queue consumed by
 *                             the worker thread (rate-limited).
 *   - DiscordNetWorker      : the worker thread run-loop (started by the client).
 */

#ifndef DISCORD_MODULE_H_
#define DISCORD_MODULE_H_

#include "Define.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Forward declarations of classes implemented in this module's sources.
class DiscordConfig;
class DiscordEmojiManager;
class DiscordGateway;
class DiscordHttp;
class DiscordClient;
class DiscordQueue;
class DiscordNetWorker;
class DiscordChatBridge;
class DiscordAdmin;
class DiscordEvents;
class DiscordPresence;
class DiscordPlayerProvider;
class DiscordMgr;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
namespace DiscordChat
{
    constexpr uint8 INVALID_RACE = 0xFF;
    constexpr uint8 INVALID_CLASS = 0xFF;

    // Discord API host and gateway.
    constexpr char const* API_HOST = "discord.com";
    constexpr char const* API_VERSION = "/api/v10";
    constexpr char const* GATEWAY_URL = "wss://gateway.discord.gg/?v=10&encoding=json";

    // WoW class ids (1-9, 11) and names used for emoji lookup and styling.
    inline std::string const& ClassName(uint8 classId)
    {
        static std::string const names[] = {
            "Unknown", "Warrior", "Paladin", "Hunter", "Rogue", "Priest",
            "DeathKnight", "Shaman", "Mage", "Warlock", "Unknown", "Druid"
        };
        static std::string const unknown = "Unknown";
        return (classId >= 1 && classId <= 11) ? names[classId] : unknown;
    }

    // WoW class colors (RGB hex) for embeds and name coloring.
    inline uint32 ClassColor(uint8 classId)
    {
        switch (classId)
        {
            case 1: return 0xC79C6E; // Warrior
            case 2: return 0xF58CBA; // Paladin
            case 3: return 0xAAD372; // Hunter
            case 4: return 0xFFF468; // Rogue
            case 5: return 0xFFFFFF; // Priest
            case 6: return 0xC41E3A; // Death Knight
            case 7: return 0x0070DD; // Shaman
            case 8: return 0x3FC7EB; // Mage
            case 9: return 0x8788EE; // Warlock
            case 11: return 0xFF7C0A; // Druid
            default: return 0x999999;
        }
    }

    // Race ids (1-based) to a stable lower-case key used in emoji config names.
    inline std::string const& RaceKey(uint8 raceId)
    {
        static std::string const keys[] = {
            "unknown", "Human", "Orc", "Dwarf", "NightElf", "Undead", "Tauren",
            "Gnome", "Troll", "BloodElf", "Draenei"
        };
        static std::string const unknown = "unknown";
        return (raceId >= 1 && raceId <= 10) ? keys[raceId] : unknown;
    }
}

// A pending outbound Discord message (command for the worker thread).
struct DiscordOutMessage
{
    uint32 id = 0;
    std::string payload;          // JSON body
    std::string path;             // REST path (with :id substitutions) or "ws" for gateway
    std::string method;           // GET / POST / PATCH / PUT / DELETE
    std::function<void(std::string const& /*jsonResult*/)> callback; // called on worker thread
    bool needsAuth = true;        // whether to attach Bot Authorization header
    uint64_t channelId = 0;       // used by webhook sends to pick webhook URL
    bool isWebhook = false;       // POST via a webhook URL (no auth header)
    bool retried = false;         // whether a 429 rate-limit retry already happened
};

// Aggregate character identity resolved for an in-game speaker.
struct DiscordPlayerIdentity
{
    std::string name;
    uint8 race = DiscordChat::INVALID_RACE;
    uint8 gender = 0;
    uint8 cls = DiscordChat::INVALID_CLASS;
    uint32 team = 0;              // TeamId: 0 alliance, 1 horde
    uint32 level = 0;
    uint32 guildId = 0;
    std::string guildName;
    bool isBot = false;
    bool valid = false;
};

// Result payload used when a Discord slash command must reply.
struct DiscordCommandResult
{
    bool reply = false;           // whether to send a reply to the interaction
    std::string content;          // reply text
    std::string embedTitle;
    std::string embedDescription;
    uint32 color = 0;
    bool ephemeral = false;
    bool needsConfirmation = false;
};

// A Discord slash-command option (one name/value pair, or a subcommand).
struct DiscordCommandOption
{
    std::string name;
    std::string value;            // string form of the value
    bool isSubcommand = false;
    bool isSubcommandGroup = false;
    std::vector<DiscordCommandOption> options; // nested options (subcommand args)

    DiscordCommandOption const* FindSubcommand(std::string const& sub) const
    {
        for (auto const& opt : options)
            if (opt.isSubcommand && opt.name == sub)
                return &opt;
        return nullptr;
    }
};

// A parsed slash-command interaction (received on the worker thread).
struct DiscordInteraction
{
    uint64 id = 0;
    uint64 applicationId = 0;
    uint64 channelId = 0;
    uint64 guildId = 0;
    uint64 userId = 0;
    std::string userTag;          // username#discriminator or username
    std::string token;
    std::vector<uint64_t> roles;  // member role snowflakes
    bool isMember = false;

    std::string name;             // top-level command name
    std::vector<DiscordCommandOption> options;

    // Helper: find the first subcommand under `name` (e.g. "kick").
    DiscordCommandOption const* FindSubcommand(std::string const& sub) const
    {
        for (auto const& opt : options)
            if (opt.isSubcommand && opt.name == sub)
                return &opt;
        return nullptr;
    }
    // Helper: read a string option from a subcommand's arguments.
    std::string GetOptionValue(DiscordCommandOption const& sub, std::string const& key) const
    {
        for (auto const& opt : sub.options)
            if (opt.name == key)
                return opt.value;
        return "";
    }
    // Helper: read a top-level option value (used by flat commands like /main).
    std::string GetTopLevelValue(std::string const& key) const
    {
        for (auto const& opt : options)
            if (opt.name == key)
                return opt.value;
        return "";
    }
};

// A Discord channel message that reached the bridge (from MESSAGE_CREATE).
struct DiscordChannelMessage
{
    uint64_t channelId = 0;
    uint64_t authorId = 0;
    std::string authorName;          // username / global_name
    bool authorIsBot = false;
    bool fromWebhook = false;
    std::string content;
    bool hasAttachments = false;
    std::string referencedAuthor;  // flattened reply target name, if any

    // Member context (present for guild messages).
    std::string nickname;            // guild nickname, if set
    std::vector<uint64_t> roles;     // member role snowflakes
    bool isInteraction = false;      // this is a slash-command response, not chat
};

// A cached Discord guild role (id, name, color, position).
struct DiscordRoleInfo
{
    uint64_t id = 0;
    std::string name;
    uint32_t color = 0;              // RGB (0xRRGGBB); 0 = default/no color
    int32_t position = 0;            // hierarchy position (higher = more important)
    bool valid = false;
};

// The resolved visual style for a Discord member (used for in-game chat).
struct DiscordMemberStyle
{
    uint32_t color = 0x5865F2;       // default: Discord blurple
    bool isAdmin = false;            // member holds an admin role (Blizz icon)
    bool hasColoredRole = false;
};

#endif // DISCORD_MODULE_H_