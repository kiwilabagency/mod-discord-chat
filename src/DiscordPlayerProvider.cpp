#include "DiscordPlayerProvider.h"
#include "DiscordMgr.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Player.h"
#include "QueryResult.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#ifdef MOD_PLAYERBOTS
#include "PlayerbotMgr.h"
#endif

#include <vector>

namespace
{
    uint32 NowSeconds()
    {
        return uint32(GameTime::GetUptime().count());
    }
}

void DiscordPlayerProvider::Initialize()
{
#ifdef MOD_PLAYERBOTS
    botsAvailable_ = true;
    LOG_INFO("modules.discord.provider", "Playerbots: Detected (mod-playerbots)");
#else
    botsAvailable_ = false;
    LOG_INFO("modules.discord.provider", "Playerbots: Not detected");
#endif
}

bool DiscordPlayerProvider::IsPlayerBot(Player const* player) const
{
#ifdef MOD_PLAYERBOTS
    if (!player)
        return false;
    return sPlayerbotsMgr.GetPlayerbotAI(const_cast<Player*>(player)) != nullptr;
#else
    (void)player;
    return false;
#endif
}

DiscordPopulation DiscordPlayerProvider::GetPopulation() const
{
    DiscordPopulation pop;
    sWorldSessionMgr->DoForAllOnlinePlayers([&](Player* player) {
        if (IsPlayerBot(player))
            ++pop.bots;
        else
            ++pop.real;
    });
    pop.total = pop.real + pop.bots;
    return pop;
}

DiscordPlayerIdentity DiscordPlayerProvider::GetIdentity(Player const* player) const
{
    DiscordPlayerIdentity identity;
    if (!player)
        return identity;

    identity.name = player->GetName();
    identity.race = player->getRace(true);
    identity.gender = player->getGender();
    identity.cls = player->getClass();
    identity.team = uint32(player->GetTeamId());
    identity.level = player->GetLevel();
    identity.guildId = player->GetGuildId();
    if (identity.guildId)
    {
        if (Guild* guild = sGuildMgr->GetGuildById(identity.guildId))
            identity.guildName = guild->GetName();
    }
    identity.isBot = IsPlayerBot(player);
    identity.valid = true;
    return identity;
}

DiscordPlayerIdentity DiscordPlayerProvider::GetIdentityForAccount(uint32 accountId, std::string const& mainChar) const
{
    DiscordPlayerIdentity identity;

    // 1. Currently-online character on the linked account.
    sWorldSessionMgr->DoForAllOnlinePlayers([&](Player* player) {
        if (!identity.valid && player->GetSession()->GetAccountId() == accountId)
            identity = GetIdentity(player);
    });
    if (identity.valid)
        return identity;

    // 2. Configured main character (cached snapshot).
    auto it = mainCache_.find(accountId);
    if (it != mainCache_.end() && NowSeconds() - it->second.second < 60)
        return it->second.first;

    // Trigger an async snapshot load for the main character.
    if (!mainChar.empty())
        RefreshMainCharacter(accountId, mainChar);

    // 3. No character identity available yet.
    return DiscordPlayerIdentity();
}

void DiscordPlayerProvider::RefreshMainCharacter(uint32 accountId, std::string const& mainChar) const
{
    // Query the characters DB for the main character's identity (async; the
    // callback runs on the game thread and updates the cache).
    CharacterDatabase.AsyncQuery(Acore::StringFormat(
        "SELECT guid, race, class, gender, level, guildid FROM characters "
        "WHERE account = {} AND name = '{}'", accountId, mainChar))
        .WithCallback([this, accountId, mainChar](QueryResult result) {
            if (!result)
                return;
            Field* f = result->Fetch();
            DiscordPlayerIdentity identity;
            identity.name = mainChar;
            identity.race = f[1].Get<uint8>();
            identity.cls = f[2].Get<uint8>();
            identity.gender = f[3].Get<uint8>();
            identity.level = f[4].Get<uint32>();
            identity.guildId = f[5].Get<uint32>();
            if (identity.guildId)
            {
                if (Guild* guild = sGuildMgr->GetGuildById(identity.guildId))
                    identity.guildName = guild->GetName();
            }
            identity.team = uint32(Player::TeamIdForRace(identity.race));
            identity.valid = true;
            mainCache_[accountId] = {identity, NowSeconds()};
        });
}