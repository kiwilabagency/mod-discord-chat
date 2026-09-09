#include "DiscordClient.h"
#include "DiscordConfig.h"
#include "DiscordJson.h"
#include "DiscordNetWorker.h"
#include "Log.h"

DiscordClient::DiscordClient(DiscordConfig const* config)
    : config_(config), worker_(std::make_shared<DiscordNetWorker>())
{
    token_ = config_->Token;
    intents_ = config_->Intents;
    logHttp_ = config_->DebugLogHttp;
}

DiscordClient::~DiscordClient()
{
    Stop();
}

void DiscordClient::Start()
{
    worker_->SetClient(shared_from_this());
    worker_->Start();
}

void DiscordClient::Stop()
{
    if (worker_)
        worker_->Stop();
}

void DiscordClient::PostChannelMessage(uint64_t channelId, std::string const& json)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = json;
    msg.path = "/channels/" + std::to_string(channelId) + "/messages";
    msg.method = "POST";
    msg.needsAuth = true;
    worker_->Queue(std::move(msg));
}

void DiscordClient::PostChannelMessageWait(uint64_t channelId, std::string const& json,
                                           std::function<void(std::string const&)> onDone)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = json;
    msg.path = "/channels/" + std::to_string(channelId) + "/messages";
    msg.method = "POST";
    msg.needsAuth = true;
    msg.callback = std::move(onDone);
    worker_->Queue(std::move(msg));
}

void DiscordClient::PatchMessage(uint64_t channelId, uint64_t messageId, std::string const& json)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = json;
    msg.path = "/channels/" + std::to_string(channelId) + "/messages/" + std::to_string(messageId);
    msg.method = "PATCH";
    msg.needsAuth = true;
    worker_->Queue(std::move(msg));
}

void DiscordClient::EditMessage(uint64_t channelId, uint64_t messageId, std::string const& json)
{
    PatchMessage(channelId, messageId, json);
}

void DiscordClient::DeleteMessage(uint64_t channelId, uint64_t messageId)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.path = "/channels/" + std::to_string(channelId) + "/messages/" + std::to_string(messageId);
    msg.method = "DELETE";
    msg.needsAuth = true;
    worker_->Queue(std::move(msg));
}

void DiscordClient::PostWebhook(std::string const& webhookUrl, std::string const& json)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = json;
    msg.path = webhookUrl;
    msg.method = "POST";
    msg.needsAuth = false;
    msg.isWebhook = true;
    worker_->Queue(std::move(msg));
}

void DiscordClient::PostWebhookWait(std::string const& webhookUrl, std::string const& json,
                                    std::function<void(std::string const&)> onDone)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = json;
    msg.path = webhookUrl + "?wait=true";
    msg.method = "POST";
    msg.needsAuth = false;
    msg.isWebhook = true;
    msg.callback = std::move(onDone);
    worker_->Queue(std::move(msg));
}

void DiscordClient::PostInteractionResponse(uint64_t interactionId, std::string const& interactionToken,
                                            std::string const& json)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = json;
    msg.path = "/interactions/" + std::to_string(interactionId) + "/" + interactionToken + "/callback";
    msg.method = "POST";
    msg.needsAuth = true;
    worker_->Queue(std::move(msg));
}

void DiscordClient::CreateInteractionFollowup(uint64_t applicationId, std::string const& interactionToken,
                                              std::string const& json)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = json;
    msg.path = "/webhooks/" + std::to_string(applicationId) + "/" + interactionToken;
    msg.method = "POST";
    msg.needsAuth = true;
    worker_->Queue(std::move(msg));
}

void DiscordClient::DeleteInteractionMessage(uint64_t applicationId, std::string const& interactionToken,
                                             uint64_t messageId)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = "";
    msg.path = "/webhooks/" + std::to_string(applicationId) + "/" + interactionToken +
               "/messages/" + std::to_string(messageId);
    msg.method = "DELETE";
    msg.needsAuth = true;
    worker_->Queue(std::move(msg));
}

void DiscordClient::RegisterCommands(std::string const& jsonArray)
{
    if (!config_->ApplicationId || !config_->GuildId)
        return;
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = jsonArray;
    msg.path = "/applications/" + std::to_string(config_->ApplicationId) +
               "/guilds/" + std::to_string(config_->GuildId) + "/commands";
    msg.method = "PUT";
    msg.needsAuth = true;
    msg.callback = [](std::string const& body) {
        LOG_INFO("modules.discord.admin", "Slash command registration response ({} bytes): {}",
                 body.size(), body.substr(0, 120));
    };
    worker_->Queue(std::move(msg));
}

void DiscordClient::GetGuildEmojis(std::function<void(std::string const&)> onDone)
{
    if (!config_->GuildId)
    {
        if (onDone) onDone("[]");
        return;
    }
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.path = "/guilds/" + std::to_string(config_->GuildId) + "/emojis";
    msg.method = "GET";
    msg.needsAuth = true;
    msg.callback = std::move(onDone);
    worker_->Queue(std::move(msg));
}

void DiscordClient::GetApplicationEmojis(std::function<void(std::string const&)> onDone)
{
    if (!config_->ApplicationId)
    {
        if (onDone) onDone("[]");
        return;
    }
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.path = "/applications/" + std::to_string(config_->ApplicationId) + "/emojis";
    msg.method = "GET";
    msg.needsAuth = true;
    msg.callback = std::move(onDone);
    worker_->Queue(std::move(msg));
}

void DiscordClient::CreateGuildRole(std::string const& name, uint32 color,
                                    std::function<void(std::string const&)> onDone)
{
    if (!config_->GuildId)
    {
        if (onDone) onDone("");
        return;
    }
    DiscordJson::Writer w;
    w.Open();
    w.Field("name", name);
    w.Field("color", (long long)color);
    w.Close();

    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = w.out;
    msg.path = "/guilds/" + std::to_string(config_->GuildId) + "/roles";
    msg.method = "POST";
    msg.needsAuth = true;
    msg.callback = std::move(onDone);
    worker_->Queue(std::move(msg));
}

void DiscordClient::ModifyGuildRolePosition(uint64_t roleId, int32 position)
{
    if (!config_->GuildId || !roleId)
        return;
    DiscordJson::Writer w;
    w.Open();
    w.Field("position", (long long)position);
    w.Close();

    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = w.out;
    msg.path = "/guilds/" + std::to_string(config_->GuildId) + "/roles/" + std::to_string(roleId);
    msg.method = "PATCH";
    msg.needsAuth = true;
    worker_->Queue(std::move(msg));
}

void DiscordClient::CreateChannelWebhook(uint64_t channelId, std::string const& name,
                                         std::string const& avatarDataUri,
                                         std::function<void(std::string const&)> onDone)
{
    if (!channelId)
    {
        if (onDone) onDone("");
        return;
    }
    DiscordJson::Writer w;
    w.Open();
    w.Field("name", name);
    if (!avatarDataUri.empty())
        w.Field("avatar", avatarDataUri);
    w.Close();

    DiscordOutMessage msg;
    msg.id = NextId();
    msg.payload = w.out;
    msg.path = "/channels/" + std::to_string(channelId) + "/webhooks";
    msg.method = "POST";
    msg.needsAuth = true;
    msg.callback = std::move(onDone);
    worker_->Queue(std::move(msg));
}

void DiscordClient::GetGuildRoles(std::function<void(std::string const&)> onDone)
{
    if (!config_->GuildId)
    {
        if (onDone) onDone("[]");
        return;
    }
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.path = "/guilds/" + std::to_string(config_->GuildId) + "/roles";
    msg.method = "GET";
    msg.needsAuth = true;
    msg.callback = std::move(onDone);
    worker_->Queue(std::move(msg));
}

void DiscordClient::GetChannel(uint64_t channelId, std::function<void(std::string const&)> onDone)
{
    DiscordOutMessage msg;
    msg.id = NextId();
    msg.path = "/channels/" + std::to_string(channelId);
    msg.method = "GET";
    msg.needsAuth = true;
    msg.callback = std::move(onDone);
    worker_->Queue(std::move(msg));
}

void DiscordClient::UpdatePresence(std::string const& json)
{
    PublishGateway(json);
}

void DiscordClient::SetGatewayEventHandler(std::function<void(DiscordGatewayEvent const&)> handler)
{
    worker_->SetGatewayEventHandler(std::move(handler));
}

void DiscordClient::PublishGateway(std::string const& payload)
{
    worker_->Publish(payload);
}