#ifndef DISCORD_EVENTS_H_
#define DISCORD_EVENTS_H_

#include "DiscordModule.h"
#include <string>

class AchievementEntry;
class ChatHandler;
class Creature;
class Player;

// Event logging into a dedicated Discord events channel. Each category is
// independently configurable; normal creature/BG kills and Playerbot activity
// are not posted unless explicitly enabled. Game-thread owned.
class DiscordEvents
{
public:
    void Initialize();
    void Shutdown();

    bool Enabled() const;

    // Post a compact single-line message (or a small embed for achievements).
    void PostEvent(std::string const& text, uint32 color = 0, bool asEmbed = false);

    // ---- Game events --------------------------------------------------------
    void OnPlayerLogin(Player* player);
    void OnPlayerLogout(Player* player);
    void OnAchievementComplete(Player* player, AchievementEntry const* achievement);
    void OnCreatureKill(Player* killer, Creature* killed);
    void OnCommandExecuted(ChatHandler* handler, std::string_view cmdStr);

    // ---- Discord member events ----------------------------------------------
    void OnMemberJoin(std::string const& userName, uint64_t userId);
    void OnMemberLeave(std::string const& userName, uint64_t userId);

    // ---- Server lifecycle ---------------------------------------------------
    void OnServerStartup();
    void OnServerShutdown(bool restart);

private:
    bool IsBossKillEnabled(Creature* killed) const;
    bool initialized_ = false;
};

#endif // DISCORD_EVENTS_H_