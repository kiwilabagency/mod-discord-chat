#ifndef DISCORD_EMOJI_MANAGER_H_
#define DISCORD_EMOJI_MANAGER_H_

#include "DiscordModule.h"
#include "DiscordConfig.h"
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Resolves the configured emoji names (faction/class/race) against the guild's
// actual custom emoji list, caches the results, and renders emoji prefixes for
// in-game speakers. All state is owned by the game thread.
//
// Resolution flow:
//   1. GET /guilds/{id}/emojis (worker thread) returns the guild emoji list.
//   2. The response is marshalled back to the game thread, where a name -> emoji
//      map is built and every configured name is resolved to its real Discord
//      custom emoji (<:name:id>).
//   3. Missing names are reported once at startup and gracefully skipped at
//      render time (never printed as broken placeholders).
class DiscordEmojiManager
{
public:
    void Initialize();
    void Refresh();
    void PrintStartupDiagnostics() const;

    bool Enabled() const { return config_ && config_->EmojisEnable; }

    // Render the emoji prefix for a resolved character identity. Emojis that are
    // disabled, or names that could not be resolved, are simply omitted.
    std::string RenderPrefix(DiscordPlayerIdentity const& identity) const;

    // Render a single category.
    std::string RenderFaction(uint32 team) const;
    std::string RenderClass(uint8 classId) const;
    std::string RenderRace(uint8 raceId, uint8 gender) const;
    std::string RenderGm() const;

    // True if the guild emoji list has been loaded at least once.
    bool HasGuildEmojis() const { return fetched_; }

    // True if the text contains a custom emoji that came from the bot application
    // (not from the guild). Discord webhooks cannot render such emojis, so the
    // bridge posts those messages as the bot instead.
    bool ContainsAppEmoji(std::string const& text) const;

    // Rewrite any app-owned emoji markup to the same-named guild emoji when one
    // exists, so webhook posts prefer guild-renderable emojis.
    std::string RewriteAppEmojisToGuild(std::string const& text) const;

    // Internal: process the emoji JSON (marshalled to game thread).
    void ProcessGuildEmojis(std::string const& json);
    void ProcessApplicationEmojis(std::string const& json);
    void ProcessEmojiUpdate(std::string const& json); // GUILD_EMOJIS_UPDATE payload

private:
    void ResolveAll();
    std::string ResolveConfigured(std::string const& configured) const;
    bool IsAppOnlyEmojiId(uint64_t id) const;
    static std::string NormalizeName(std::string name);

    DiscordConfig const* config_ = nullptr;

    bool fetched_ = false;
    std::unordered_map<std::string, std::string> nameToEmoji_; // lower-name -> <:name:id>
    std::unordered_map<std::string, std::string> unresolved_;  // configured name -> reason (diagnostics)

    std::set<uint64_t> appEmojiIds_;   // emoji IDs owned by the bot application
    std::set<uint64_t> guildEmojiIds_; // emoji IDs in the guild
    uint32 appEmojiCount_ = 0;

    std::string resolvedFaction_[2];                                // [team] -> emoji or empty
    std::string resolvedGm_;                                      // Blizz/GM badge emoji or empty
    std::unordered_map<uint8, std::string> resolvedClass_;          // class id -> emoji or empty
    std::unordered_map<uint8, std::pair<std::string, std::string>> resolvedRace_; // race id -> {male,female}

    uint32 resolvedClassCount_ = 0;
    uint32 resolvedRaceCount_ = 0;
    uint32 resolvedFactionCount_ = 0;
};

#endif // DISCORD_EMOJI_MANAGER_H_