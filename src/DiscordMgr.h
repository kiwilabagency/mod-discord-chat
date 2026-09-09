#ifndef DISCORD_MGR_H_
#define DISCORD_MGR_H_

#include "DiscordModule.h"
#include "DiscordConfig.h"
#include "DiscordGateway.h"
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace DiscordJson { struct Value; }

class DiscordClient;
class DiscordEmojiManager;
class DiscordPlayerProvider;
class DiscordChatBridge;
class DiscordAdmin;
class DiscordEvents;
class DiscordPresence;
class DiscordServerStatus;
class DiscordFilter;

// Central game-thread singleton for mod-discord-chat.
//
// Threading model:
//   - All Discord networking runs on the DiscordNetWorker thread.
//   - Gateway events and HTTP callbacks are marshalled onto the game thread
//     through a mutex-protected queue drained by DiscordMgr::Update (which the
//     worldserver tick calls via WorldScript::OnUpdate).
//   - The game thread never blocks on Discord; outbound messages are enqueued.
class DiscordMgr
{
public:
    static DiscordMgr* instance();  // defined in DiscordMgr.cpp

    void Initialize();        // OnAfterConfigLoad(false) - first boot
    void ReloadConfig();      // /config reload or OnAfterConfigLoad(true)
    void Shutdown();          // OnShutdown
    void Update(uint32 diff); // OnUpdate

    bool IsEnabled() const { return enabled_ && initialized_; }

    // Marshals work from any thread onto the game thread.
    void PostToGameThread(std::function<void()>&& fn);

    // Marshals work onto the game thread after a delay (milliseconds).
    void PostToGameThreadDelayed(uint32 ms, std::function<void()> fn);

    // Accessors.
    DiscordConfig const* Config() const { return &config_; }
    DiscordClient* Client() const { return client_.get(); }
    DiscordEmojiManager* Emojis() const { return emojis_.get(); }
    DiscordPlayerProvider* PlayerProvider() const { return playerProvider_.get(); }
    DiscordChatBridge* ChatBridge() const { return bridge_.get(); }
    DiscordAdmin* Admin() const { return admin_.get(); }
    DiscordEvents* Events() const { return events_.get(); }
    DiscordPresence* Presence() const { return presence_.get(); }
    DiscordServerStatus* ServerStatus() const { return serverStatus_.get(); }
    DiscordFilter* Filter() const { return filter_.get(); }

    std::string BuildStatusText(bool includeLastUpdate) const;
    static std::string FormatUptime(uint32 seconds);

    std::string const& GetGuildName() const { return guildName_; }

    // Discord role handling (game thread).
    void RefreshRoles();
    void ProcessGuildRoles(std::string const& json);
    DiscordMemberStyle ResolveMemberStyle(std::vector<uint64_t> const& roles) const;
    DiscordRoleInfo const* GetRoleInfo(uint64_t roleId) const;
    std::vector<std::string> ResolveMemberTags(std::vector<uint64_t> const& roles) const;
    void ResolveMemberIcons(std::vector<uint64_t> const& roles, std::string& factionIcon,
                            std::string& classIcon, uint8& classId) const;
    std::unordered_map<uint64_t, DiscordRoleInfo> const& GetRoleCache() const { return roleCache_; }

private:
    DiscordMgr() = default;
    ~DiscordMgr();  // defined in DiscordMgr.cpp where subsystem types are complete
    DiscordMgr(DiscordMgr const&) = delete;
    DiscordMgr& operator=(DiscordMgr const&) = delete;

    void ProcessGameQueue();
    void ProcessDelayedTasks();
    void HandleGatewayEvent(DiscordGatewayEvent const& ev);
    void PrintStartupDiagnostics();

    // Event handlers (run on the game thread).
    void HandleReady(DiscordJson::Value const& d);
    void HandleMessageCreate(DiscordJson::Value const& d);
    void HandleInteractionCreate(DiscordJson::Value const& d);
    void HandleMemberEvent(std::string const& eventName, DiscordJson::Value const& d);

    // Parsing helpers.
    static void ParseInteraction(DiscordJson::Value const& d, DiscordInteraction& out);
    static void ParseChannelMessage(DiscordJson::Value const& d, DiscordChannelMessage& out);
    static void ParseOptions(DiscordJson::Value const& opts, std::vector<DiscordCommandOption>& out);

    DiscordConfig config_;
    std::shared_ptr<DiscordClient> client_;

    std::unique_ptr<DiscordEmojiManager> emojis_;
    std::unique_ptr<DiscordPlayerProvider> playerProvider_;
    std::unique_ptr<DiscordChatBridge> bridge_;
    std::unique_ptr<DiscordAdmin> admin_;
    std::unique_ptr<DiscordEvents> events_;
    std::unique_ptr<DiscordPresence> presence_;
    std::unique_ptr<DiscordServerStatus> serverStatus_;
    std::unique_ptr<DiscordFilter> filter_;

    // Worker -> game thread marshalling.
    std::mutex queueMutex_;
    std::deque<std::function<void()>> queue_;

    bool enabled_ = false;
    bool initialized_ = false;
    std::string guildName_;
    std::unordered_map<uint64_t, DiscordRoleInfo> roleCache_; // guild role id -> info

    struct DelayedTask
    {
        uint32 fireMs = 0;
        std::function<void()> fn;
    };
    std::mutex delayedMutex_;
    std::vector<DelayedTask> delayedTasks_;
};

#define sDiscordMgr DiscordMgr::instance()

#endif // DISCORD_MGR_H_