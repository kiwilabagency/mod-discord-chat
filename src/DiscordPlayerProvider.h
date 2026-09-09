#ifndef DISCORD_PLAYER_PROVIDER_H_
#define DISCORD_PLAYER_PROVIDER_H_

#include "DiscordModule.h"
#include <cstdint>
#include <map>
#include <string>

class Player;

struct DiscordPopulation
{
    uint32 real = 0;
    uint32 bots = 0;
    uint32 total = 0;
};

// Provides population numbers (with playerbot separation) and character
// identity resolution. Game-thread owned.
class DiscordPlayerProvider
{
public:
    void Initialize();

    DiscordPopulation GetPopulation() const;

    // True when mod-playerbots is compiled in and detected.
    bool PlayerbotsAvailable() const { return botsAvailable_; }
    bool IsPlayerBot(Player const* player) const;

    // Build a Discord identity from a live in-game player.
    DiscordPlayerIdentity GetIdentity(Player const* player) const;

    // Resolve the WoW identity used for a linked Discord user:
    //   1. their currently-online character
    //   2. their configured main character (cached snapshot)
    //   3. no valid identity (invalid struct)
    // May return an identity that is not yet valid if the main-character
    // snapshot is still loading asynchronously.
    DiscordPlayerIdentity GetIdentityForAccount(uint32 accountId, std::string const& mainChar) const;

    // Refresh the cached main-character snapshot for an account (async, game thread).
    void RefreshMainCharacter(uint32 accountId, std::string const& mainChar) const;

private:
    void RefreshMainCharacterCallback(uint32 accountId, std::string const& mainChar) const;

    bool botsAvailable_ = false;
    mutable std::map<uint32, std::pair<DiscordPlayerIdentity, uint32>> mainCache_; // accountId -> (identity, lastRefreshSecs)
};

#endif // DISCORD_PLAYER_PROVIDER_H_