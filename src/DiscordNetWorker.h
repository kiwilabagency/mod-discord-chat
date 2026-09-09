#ifndef DISCORD_NET_WORKER_H_
#define DISCORD_NET_WORKER_H_

#include "DiscordModule.h"
#include "DiscordQueue.h"
#include "DiscordHttp.h"
#include "DiscordGateway.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <unordered_map>
#include <functional>

// Drives all Discord networking on a dedicated worker thread. It owns the
// outbound queue, the HTTP client, rate-limit state and the gateway websocket.
// The game thread enqueues messages (fast, non-blocking); this worker executes
// them and receives gateway events, which are forwarded back onto the game
// thread via the DiscordMgr callback for processing.
class DiscordNetWorker
{
public:
    DiscordNetWorker();
    ~DiscordNetWorker();

    void Start();
    void Stop();

    void Queue(DiscordOutMessage msg);

    // Called from the worker thread when a gateway event arrives. The game
    // thread's DiscordMgr registers this handler (must be thread-safe).
    void SetGatewayEventHandler(std::function<void(DiscordGatewayEvent const&)> handler);

    // Presence providers, called from the gateway thread to build activity text.
    void SetPresenceProviders(std::function<std::string()> statusText,
                              std::function<std::string()> activityType);

    // Called from the game thread to publish an outbound websocket payload
    // (e.g. presence refresh).
    void Publish(std::string const& payload);

    // Forward a gateway event (called from the gateway thread).
    void DispatchEvent(std::string const& name, std::string const& data);

    DiscordHttp& Http() { return http_; }

    void SetClient(std::shared_ptr<DiscordClient> client) { client_ = client; }
    std::shared_ptr<DiscordClient> Client() const { return client_.lock(); }

    bool IsGatewayConnected() const;

private:
    void Run();
    void HandleHttpMessage(DiscordOutMessage& msg);

    DiscordQueue queue_;
    DiscordHttp http_;
    std::thread thread_;
    std::thread gatewayThread_;
    std::atomic<bool> running_{false};

    // Gateway
    std::shared_ptr<DiscordGateway> gateway_;

    std::weak_ptr<DiscordClient> client_;

    std::function<void(DiscordGatewayEvent const&)> gatewayHandler_;
    std::function<std::string()> statusTextProvider_;
    std::function<std::string()> activityTypeProvider_;
};

#endif // DISCORD_NET_WORKER_H_