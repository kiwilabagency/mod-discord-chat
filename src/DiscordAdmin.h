#ifndef DISCORD_ADMIN_H_
#define DISCORD_ADMIN_H_

#include "DiscordModule.h"
#include <map>
#include <string>
#include <vector>

class ChatHandler;

// Discord administrative slash commands, permission control, confirmation
// buttons, raw console access and the persisted audit log.
//
// All methods run on the game thread (interactions are marshalled from the
// worker thread via DiscordMgr's game-thread queue).
class DiscordAdmin
{
public:
    struct PendingAction
    {
        std::string token;
        uint64 requesterId = 0;
        std::string command;      // e.g. "server.shutdown"
        std::string args;         // human-readable arguments
        uint32 createdAt = 0;
    };

    void Initialize();
    void Shutdown();

    // Build + register the guild slash commands (PUT applications/.../commands).
    void RegisterCommands();

    // Handle an INTERACTION_CREATE (slash command or button click).
    void HandleInteraction(DiscordInteraction const& ix);

    // Audit logging (characters DB).
    void LogAudit(DiscordInteraction const& ix, std::string const& command,
                  std::string const& args, std::string const& result);

    // Create the recommended guild roles (used by .discord setup roles).
    void CreateRolesInGame(ChatHandler* handler);

private:
    // ---- Slash command dispatch ---------------------------------------------
    void HandleCommand(DiscordInteraction const& ix);
    void HandleServerCommands(DiscordInteraction const& ix);
    void HandlePlayerCommands(DiscordInteraction const& ix);
    void HandleAccountCommands(DiscordInteraction const& ix);
    void HandleAnnounceCommands(DiscordInteraction const& ix);
    void HandleConfigCommands(DiscordInteraction const& ix);
    void HandleConsoleCommand(DiscordInteraction const& ix);
    void HandleDiscordCommands(DiscordInteraction const& ix);
    void HandleSetupCommands(DiscordInteraction const& ix);
    void CreateRoles(DiscordInteraction const& ix);

    // ---- Confirmation flow --------------------------------------------------
    void RequestConfirmation(DiscordInteraction const& ix, std::string const& command,
                             std::string const& args, std::string const& detail);
    void HandleComponentInteraction(DiscordInteraction const& ix);

    // ---- Action execution ---------------------------------------------------
    void ExecuteAction(PendingAction const& action, DiscordInteraction const& confirmer);
    void ExecuteServerAction(PendingAction const& action, DiscordInteraction const& confirmer);
    void ExecutePlayerAction(PendingAction const& action, DiscordInteraction const& confirmer);
    void ExecuteAccountAction(PendingAction const& action, DiscordInteraction const& confirmer);
    void ExecuteConsoleAction(PendingAction const& action, DiscordInteraction const& confirmer);

    // ---- Reply helpers ------------------------------------------------------
    void PostInteraction(DiscordInteraction const& ix, std::string const& json);
    void Reply(DiscordInteraction const& ix, std::string const& content, bool ephemeral = false);
    void ReplyEmbed(DiscordInteraction const& ix, std::string const& title,
                    std::string const& description, uint32 color, bool ephemeral = false);
    void UpdateMessage(DiscordInteraction const& ix, std::string const& content);
    void UpdateMessageWithComponents(DiscordInteraction const& ix, std::string const& content,
                                     std::string const& confirmId, std::string const& cancelId);

    // ---- Permissions --------------------------------------------------------
    bool IsAdmin(DiscordInteraction const& ix) const;
    bool CategoryEnabled(char const* group) const;

    // ---- Utility ------------------------------------------------------------
    static std::string MakeToken();
    uint32 NowSeconds() const;
    std::string FormatError(std::string const& e) const;

    std::map<std::string, PendingAction> pendingActions_;
    bool initialized_ = false;
};

#endif // DISCORD_ADMIN_H_