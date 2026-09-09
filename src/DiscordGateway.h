#ifndef DISCORD_GATEWAY_H_
#define DISCORD_GATEWAY_H_

#include "DiscordModule.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <functional>

// A Discord gateway (websocket) event, marshalled from the worker thread back
// onto the game thread.
struct DiscordGatewayEvent
{
    std::string name;      // event name e.g. "MESSAGE_CREATE", "READY"
    std::string data;      // raw JSON payload (the full envelope)
};

// Gateway client. Runs inside its own thread (spawned by DiscordNetWorker).
// It connects to the Discord gateway websocket, performs identify/heartbeat,
// maintains presence, and dispatches events through the provided handler.
//
// Threading:
//   - reader loop  : this thread  (reads frames, heartbeats, dispatches events)
//   - writer loop  : helper thread (drains the outbound publish queue)
//   - game thread  : never touches the socket; only calls Publish()/Stop().
class DiscordGateway
{
public:
    DiscordGateway();
    ~DiscordGateway();

    // Runs until Stop() is called or a fatal connection error occurs.
    void Run(std::string const& token, uint32 intents,
             std::function<void(std::string const& name, std::string const& data)> eventHandler,
             std::function<std::string()> statusTextProvider,
             std::function<std::string()> presenceTypeProvider);

    void Stop();

    // Queue an outbound websocket payload (e.g. presence op 3). Thread-safe.
    void Publish(std::string const& payload);

    // Connection state.
    bool IsConnected() const { return connected_.load(); }
    void SetConnected(bool v) { connected_.store(v); }

private:
    struct GwConn;

    void ConnectOnce();
    void ReadLoop(std::shared_ptr<GwConn> conn);
    void WriteLoop(std::shared_ptr<GwConn> conn);
    void SendText(std::string const& payload);
    std::string MakePresencePayload() const;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::string token_;
    uint32 intents_ = 0;
    std::function<void(std::string const&, std::string const&)> eventHandler_;
    std::function<std::string()> statusTextProvider_;
    std::function<std::string()> presenceTypeProvider_;

    // Current connection (used by SendText).
    std::mutex connMutex_;
    std::shared_ptr<GwConn> conn_;

    // Outbound publish queue (reader -> writer).
    std::mutex pubMutex_;
    std::condition_variable pubCv_;
    std::queue<std::string> publishQueue_;
    bool wake_ = false;
};

#endif // DISCORD_GATEWAY_H_