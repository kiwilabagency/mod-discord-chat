#include "DiscordServerStatus.h"
#include "DiscordClient.h"
#include "DiscordConfig.h"
#include "DiscordJson.h"
#include "DiscordMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"
#include "StringFormat.h"

#include <cstdlib>

namespace
{
    std::string MakeJson(std::string const& v)
    {
        return "\"" + DiscordJson::Escape(v) + "\"";
    }
}

void DiscordServerStatus::Initialize()
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    enabled_ = cfg && cfg->StatusEnable && cfg->StatusChannelId;
    channelId_ = cfg ? cfg->StatusChannelId : 0;
    interval_ = cfg ? cfg->StatusUpdateInterval : 60;
    timer_ = 0;
    messageId_ = cfg ? cfg->StatusMessageId : 0;

    if (!enabled_)
        return;

    // Load any previously-created status message ID from the DB so the same
    // message is edited again after a restart.
    CharacterDatabase.AsyncQuery(Acore::StringFormat(
        "SELECT channel_id, message_id FROM mod_discord_status WHERE realm_id = {}",
        cfg ? cfg->GameRealmId : 0))
        .WithCallback([this](QueryResult result) {
            if (result)
            {
                Field* f = result->Fetch();
                if (messageId_ == 0)
                    messageId_ = std::strtoull(f[1].Get<std::string>().c_str(), nullptr, 10);
            }
        });
}

void DiscordServerStatus::Update(uint32 diff)
{
    if (!enabled_)
        return;
    timer_ += diff;
    if (timer_ >= interval_ * 1000)
    {
        timer_ = 0;
        SendOrUpdate();
    }
}

void DiscordServerStatus::SendOrUpdate()
{
    auto client = sDiscordMgr->Client();
    DiscordConfig const* cfg = sDiscordMgr->Config();
    if (!client || !cfg)
        return;

    std::string text = sDiscordMgr->BuildStatusText(true);

    DiscordJson::Writer w;
    w.Open();
    w.Key("embeds");
    w.out += "[";
    DiscordJson::Writer e;
    e.Open();
    e.Field("title", cfg->ServerName + " - Server Status");
    e.Field("description", text);
    e.Field("color", 0x00AE86LL);
    if (!cfg->ServerIconUrl.empty())
        e.Raw("thumbnail", "{\"url\":" + MakeJson(cfg->ServerIconUrl) + "}");
    e.Close();
    w.out += e.out;
    w.out += "]";
    w.Close();

    if (messageId_ == 0)
    {
        // Create the message and remember its ID.
        client->PostChannelMessageWait(channelId_, w.out, [this](std::string const& json) {
            DiscordJson::Value parsed;
            if (DiscordJson::Parse(json, parsed))
            {
                uint64_t newId = uint64_t(DiscordJson::GetInt(parsed, "id"));
                if (newId)
                    sDiscordMgr->PostToGameThread([this, newId]() {
                        messageId_ = newId;
                        DiscordConfig const* cfg = sDiscordMgr->Config();
                        CharacterDatabase.Execute(
                            "REPLACE INTO mod_discord_status (realm_id, channel_id, message_id) VALUES ({}, '{}', '{}')",
                            cfg ? cfg->GameRealmId : 0, std::to_string(channelId_), std::to_string(newId));
                    });
            }
        });
    }
    else
    {
        client->PatchMessage(channelId_, messageId_, w.out);
    }
}