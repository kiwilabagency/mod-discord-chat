#include "GlobalChatProvider.h"
#include "DiscordConfig.h"
#include "DiscordMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "QueryResult.h"
#include "WorldSessionMgr.h"

#include <unordered_set>

namespace
{
    void BroadcastToFaction(std::string const& text, TeamId faction)
    {
        if (faction == TEAM_NEUTRAL)
        {
            sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, text, nullptr);
            return;
        }
        sWorldSessionMgr->DoForAllOnlinePlayers([&](Player* player) {
            if (player->GetTeamId() == faction)
                sWorldSessionMgr->SendServerMessage(SERVER_MSG_STRING, text, player);
        });
    }
}

void AzerothCoreGlobalChatProvider::SendToGame(std::string const& text, TeamId faction) const
{
    BroadcastToFaction(text, faction);
}

void WorldChatGlobalChatProvider::SendToGame(std::string const& text, TeamId faction) const
{
    // mod-world-chat uses a "World" channel; fall back to a faction-aware
    // broadcast so the message is always visible to Global Chat participants.
    BroadcastToFaction(text, faction);
}

void GozzimGlobalChatProvider::SendToGame(std::string const& text, TeamId faction) const
{
    BroadcastToFaction(text, faction);
}

void GlobalChatProviderMgr::Initialize()
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!cfg || !cfg->GlobalChatEnable)
    {
        provider_.reset();
        selectedName_ = "none";
        return;
    }

    Detect();

    std::string want = cfg->Provider;
    std::string wantLower;
    for (char c : want)
        wantLower += char(std::tolower(static_cast<unsigned char>(c)));

    if (wantLower == "none")
    {
        provider_.reset();
        selectedName_ = "none";
        LOG_INFO("modules.discord.provider", "Global Chat Provider: disabled (none)");
        return;
    }

    // Explicit provider selection.
    if (wantLower != "auto")
    {
        if (wantLower == "azerothcore")
            provider_ = std::make_unique<AzerothCoreGlobalChatProvider>();
        else if (wantLower == "world-chat" || wantLower == "worldchat")
            provider_ = std::make_unique<WorldChatGlobalChatProvider>();
        else if (wantLower == "gozzim")
            provider_ = std::make_unique<GozzimGlobalChatProvider>();
        else
        {
            LOG_WARN("modules.discord.provider", "Discord.GlobalChat.Provider '{}' is not recognized. "
                     "Allowed: AUTO | azerothcore | world-chat | gozzim | none. Falling back to AUTO.", cfg->Provider);
            // fall through to auto
        }

        if (provider_)
        {
            if (!provider_->GetCommandName().empty())
            {
                LOG_INFO("modules.discord.provider", "Global Chat Provider: {} (selected)", provider_->GetName());
                selectedName_ = provider_->GetName();
                return;
            }
            provider_.reset();
        }
    }

    if (!provider_)
    {
        LOG_INFO("modules.discord.provider", "Global Chat Provider: AUTO - none detected. "
                 "Install one of azerothcore/mod-global-chat, azerothcore/mod-world-chat or Gozzim/mod-globalchat "
                 "to bridge game chat, or set Discord.GlobalChat.Provider explicitly.");
        selectedName_ = "none";
    }
    else
    {
        LOG_INFO("modules.discord.provider", "Global Chat Provider: {} (auto-detected)", provider_->GetName());
        selectedName_ = provider_->GetName();
    }
}

void GlobalChatProviderMgr::Detect()
{
    // Detect installed providers by checking the world DB command table.
    // azerothcore/mod-global-chat adds 'chat', mod-world-chat adds 'world',
    // Gozzim/mod-globalchat adds 'global'.
    std::unordered_set<std::string> installed;
    if (QueryResult result = WorldDatabase.Query(
            "SELECT name FROM command WHERE name IN ('chat','world','global')"))
    {
        do
        {
            Field* f = result->Fetch();
            installed.insert(f[0].Get<std::string>());
        } while (result->NextRow());
    }

    if (installed.count("chat"))
    {
        provider_ = std::make_unique<AzerothCoreGlobalChatProvider>();
        return;
    }
    if (installed.count("global"))
    {
        provider_ = std::make_unique<GozzimGlobalChatProvider>();
        return;
    }
    if (installed.count("world"))
    {
        provider_ = std::make_unique<WorldChatGlobalChatProvider>();
        return;
    }
    provider_.reset();
}