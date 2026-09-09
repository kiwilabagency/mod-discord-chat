#ifndef GLOBAL_CHAT_PROVIDER_H_
#define GLOBAL_CHAT_PROVIDER_H_

#include "DiscordModule.h"
#include "SharedDefines.h"
#include <memory>
#include <string>
#include <vector>

// Abstraction over the supported in-game Global Chat modules. The Discord
// bridge talks only to this interface; it never depends on one provider.
//
//   azerothcore/mod-global-chat  -> command ".chat"
//   azerothcore/mod-world-chat   -> command ".world" / "World" channel
//   Gozzim/mod-globalchat        -> command ".global" / "Global" channel
//
// Providers are auto-detected at startup (via the world DB command table) or
// selected explicitly with Discord.GlobalChat.Provider.
class GlobalChatProvider
{
public:
    virtual ~GlobalChatProvider() = default;

    virtual std::string GetName() const = 0;             // e.g. "azerothcore"
    virtual std::string GetCommandName() const = 0;      // e.g. "chat" ("" = none)
    virtual std::string GetChannelName() const { return ""; } // e.g. "World" ("" = none)

    // True when players must opt in (e.g. ".chat on") to receive global chat.
    // For such providers the bridge only delivers to players who opted in.
    virtual bool IsOptInOnly() const { return false; }

    // Broadcast a Discord message into the game. `faction` filters recipients
    // when split-faction channels are used (TEAM_NEUTRAL = everyone).
    virtual void SendToGame(std::string const& text, TeamId faction) const = 0;
};

class GlobalChatProviderMgr
{
public:
    void Detect();

    // Resolve the active provider (AUTO detection or explicit config).
    void Initialize();

    GlobalChatProvider const* GetProvider() const { return provider_.get(); }
    std::string const& GetSelectedName() const { return selectedName_; }
    bool IsEnabled() const { return provider_ != nullptr; }

    static GlobalChatProviderMgr* instance()
    {
        static GlobalChatProviderMgr mgr;
        return &mgr;
    }

private:
    std::unique_ptr<GlobalChatProvider> provider_;
    std::string selectedName_;
};

#define sGlobalChatProviderMgr GlobalChatProviderMgr::instance()

// Concrete providers.
class AzerothCoreGlobalChatProvider final : public GlobalChatProvider
{
public:
    std::string GetName() const override { return "azerothcore"; }
    std::string GetCommandName() const override { return "chat"; }
    bool IsOptInOnly() const override { return true; }
    void SendToGame(std::string const& text, TeamId faction) const override;
};

class WorldChatGlobalChatProvider final : public GlobalChatProvider
{
public:
    std::string GetName() const override { return "world-chat"; }
    std::string GetCommandName() const override { return "world"; }
    std::string GetChannelName() const override { return "World"; }
    void SendToGame(std::string const& text, TeamId faction) const override;
};

class GozzimGlobalChatProvider final : public GlobalChatProvider
{
public:
    std::string GetName() const override { return "gozzim"; }
    std::string GetCommandName() const override { return "global"; }
    std::string GetChannelName() const override { return "Global"; }
    void SendToGame(std::string const& text, TeamId faction) const override;
};

#endif // GLOBAL_CHAT_PROVIDER_H_