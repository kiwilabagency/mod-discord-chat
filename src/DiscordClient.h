#ifndef DISCORD_CLIENT_H_
#define DISCORD_CLIENT_H_

#include "DiscordModule.h"
#include "DiscordNetWorker.h"
#include "DiscordGateway.h"
#include "DiscordHttp.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <functional>

// Owns the DiscordNetWorker (and thus the worker thread + gateway). Exposes a
// game-thread friendly API: all methods just enqueue outbound messages and
// return immediately (non-blocking). Results/callbacks execute on the worker
// thread.
class DiscordClient : public std::enable_shared_from_this<DiscordClient>
{
public:
    explicit DiscordClient(DiscordConfig const* config);
    ~DiscordClient();

    void Start();
    void Stop();

    // Thread-safe connection status.
    void SetConnected(bool connected) { connected_ = connected; }
    bool IsConnected() const { return connected_.load(); }

    // Configuration accessors (read by the worker thread).
    std::string const& Token() const { return token_; }
    uint32 Intents() const { return intents_; }
    bool HasToken() const { return !token_.empty(); }
    bool LogHttp() const { return logHttp_; }

    // ---- REST helpers (non-blocking, fire-and-forget) ---------------------
    void PostChannelMessage(uint64_t channelId, std::string const& json);
    void PostChannelMessageWait(uint64_t channelId, std::string const& json,
                                std::function<void(std::string const&)> onDone);
    void PatchMessage(uint64_t channelId, uint64_t messageId, std::string const& json);
    void PostWebhook(std::string const& webhookUrl, std::string const& json);
    void PostWebhookWait(std::string const& webhookUrl, std::string const& json,
                         std::function<void(std::string const&)> onDone);
    void PostInteractionResponse(uint64_t interactionId, std::string const& interactionToken,
                                 std::string const& json);
    void CreateInteractionFollowup(uint64_t applicationId, std::string const& interactionToken,
                                   std::string const& json);
    void DeleteMessage(uint64_t channelId, uint64_t messageId);
    void DeleteInteractionMessage(uint64_t applicationId, std::string const& interactionToken,
                                  uint64_t messageId);
    void EditMessage(uint64_t channelId, uint64_t messageId, std::string const& json);
    void RegisterCommands(std::string const& jsonArray);
    void GetGuildEmojis(std::function<void(std::string const&)> onDone);
    void GetApplicationEmojis(std::function<void(std::string const&)> onDone);
    void GetGuildRoles(std::function<void(std::string const&)> onDone);
void CreateGuildRole(std::string const& name, uint32 color,
                     std::function<void(std::string const&)> onDone);
    void ModifyGuildRolePosition(uint64_t roleId, int32 position);
    void CreateChannelWebhook(uint64_t channelId, std::string const& name, std::string const& avatarDataUri,
                              std::function<void(std::string const&)> onDone);
    void UpdatePresence(std::string const& json);

    // Fetch a channel (returns JSON via callback on worker thread).
    void GetChannel(uint64_t channelId, std::function<void(std::string const&)> onDone);

    // Gateway event handler registration (worker -> game thread).
    void SetGatewayEventHandler(std::function<void(DiscordGatewayEvent const&)> handler);
    void PublishGateway(std::string const& payload);

    DiscordNetWorker& Worker() { return *worker_; }

private:
    uint64_t NextId() { return ++nextId_; }

    DiscordConfig const* config_;
    std::shared_ptr<DiscordNetWorker> worker_;
    std::atomic<uint64_t> nextId_{1};
    std::atomic<bool> connected_{false};
    std::mutex mutex_;

    // Worker-thread readable snapshot of connection-relevant config.
    std::string token_;
    uint32 intents_ = 0;
    bool logHttp_ = false;
};

#endif // DISCORD_CLIENT_H_