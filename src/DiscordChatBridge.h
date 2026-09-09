#ifndef DISCORD_CHAT_BRIDGE_H_
#define DISCORD_CHAT_BRIDGE_H_

#include "DiscordModule.h"
#include "DiscordQueue.h"
#include "DiscordHttp.h"
#include "SharedDefines.h"
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>

class Channel;
class Player;

// Dedicated fast-path sender for chat webhooks and slash-command interaction
// responses. Runs on its own thread so these never wait behind background REST
// work (emoji/role fetches, events, status) that can delay the main worker.
// Interaction replies must arrive within Discord's ~3 second window.
class DiscordQuickSender
{
public:
    void Start();
    void Stop();
    void Send(DiscordOutMessage msg);

private:
    void Run();

    DiscordQueue queue_;
    DiscordHttp http_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

DiscordQuickSender& GetDiscordQuickSender();

// Two-way Global Chat bridge between Discord and the in-game Global Chat.
//
//   Game  -> Discord : webhook messages (per-character speaker), or a bot
//                      message with emoji prefix when no webhook is configured.
//   Discord -> Game  : "[Discord] Name: text" via the active Global Chat
//                      provider, faction-aware when split channels are used.
//
// Game-thread owned; outbound Discord traffic is queued to the worker thread
// (never blocks the game loop).
class DiscordChatBridge
{
public:
    void Initialize();
    void Shutdown();

    // ---- Discord -> Game ---------------------------------------------------
    // Called by DiscordMgr when a MESSAGE_CREATE event arrives on a chat channel.
    void HandleMessageCreate(DiscordChannelMessage const& msg);

    // ---- Game -> Discord ---------------------------------------------------
    // Capture: player executed a Global Chat provider command (e.g. .chat).
    void HandleGameCommand(Player* player, std::string_view cmdStr);
    // Capture: player spoke in a provider chat channel.
    void HandleChannelChat(Player* player, std::string const& channelName, std::string const& msg);
    // Capture: our own .discord say command.
    void HandleDiscordSay(Player* player, std::string const& msg);

    // Opt-in state for command-based providers (.chat on / .chat off).
    void SetGlobalChatEnabled(uint32 playerGuid, bool enabled) const;
    bool IsGlobalChatEnabled(uint32 playerGuid) const;

private:
    struct PendingWebhookMessage
    {
        std::string username;
        std::string prefix;
        std::string message;
        uint32 color = 0;
    };

    void RelayToDiscord(Player* player, std::string const& message);
    void SendToChannel(uint64_t channelId, std::string const& webhookUrl,
                       std::string const& username, std::string const& prefix,
                       std::string const& message, uint32 color);
    uint64_t ResolveChatChannel(uint32 team) const;
    std::string ResolveWebhookUrl(uint32 team) const;
    void EnsureWebhook(uint64_t channelId);
    void QueuePendingWebhookMessage(uint64_t channelId, std::string const& username,
                                    std::string const& prefix, std::string const& message,
                                    uint32 color);
    void FlushPendingWebhookMessages(uint64_t channelId, std::string const& webhookUrl);

    // Discord -> game helpers.
    std::string BuildGameMessage(DiscordChannelMessage const& msg) const;
    void HandleDiscordToGame(std::string const& text, uint32 team);

    bool initialized_ = false;
    mutable std::unordered_map<std::string, uint32> recentRelays_; // "name\x01message" -> uptimeSec
    mutable std::unordered_map<uint32, bool> globalChatEnabled_;   // player low guid -> opted in (.chat on)
    mutable std::unordered_map<uint64_t, std::string> autoWebhookUrls_;   // channel -> auto-created webhook URL
    mutable std::unordered_map<uint64_t, uint32> webhookAttemptTime_;     // channel -> last creation attempt (uptime s)
    mutable std::unordered_map<uint64_t, std::vector<PendingWebhookMessage>> pendingWebhookMessages_;
};

#endif // DISCORD_CHAT_BRIDGE_H_