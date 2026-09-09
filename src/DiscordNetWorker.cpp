#include "DiscordNetWorker.h"
#include "DiscordClient.h"
#include "DiscordJson.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <thread>

DiscordNetWorker::DiscordNetWorker() = default;
DiscordNetWorker::~DiscordNetWorker() = default;

void DiscordNetWorker::SetGatewayEventHandler(std::function<void(DiscordGatewayEvent const&)> handler)
{
    gatewayHandler_ = std::move(handler);
}

void DiscordNetWorker::SetPresenceProviders(std::function<std::string()> statusText,
                                            std::function<std::string()> activityType)
{
    statusTextProvider_ = std::move(statusText);
    activityTypeProvider_ = std::move(activityType);
}

void DiscordNetWorker::Start()
{
    if (running_.exchange(true))
        return;
    thread_ = std::thread(&DiscordNetWorker::Run, this);
}

void DiscordNetWorker::Stop()
{
    if (!running_.exchange(false))
        return;

    // Stop the REST loop first; it is the one that spawns the gateway thread
    // and owns the gateway pointer.
    if (thread_.joinable())
        thread_.join();

    if (gateway_)
        gateway_->Stop();
    if (gatewayThread_.joinable())
        gatewayThread_.join();
}

void DiscordNetWorker::Queue(DiscordOutMessage msg)
{
    queue_.Push(std::move(msg));
}

void DiscordNetWorker::Publish(std::string const& payload)
{
    DiscordOutMessage msg;
    msg.payload = payload;
    msg.path = "ws";
    msg.method = "PUBLISH";
    queue_.Push(std::move(msg));
}

void DiscordNetWorker::DispatchEvent(std::string const& name, std::string const& data)
{
    DiscordGatewayEvent ev{name, data};
    if (gatewayHandler_)
    {
        try { gatewayHandler_(ev); }
        catch (std::exception const& e)
        {
            LOG_WARN("modules.discord.net", "Gateway event handler error: {}", e.what());
        }
    }
}

bool DiscordNetWorker::IsGatewayConnected() const
{
    return gateway_ && gateway_->IsConnected();
}

void DiscordNetWorker::Run()
{
    LOG_INFO("modules.discord.net", "Discord REST worker thread started.");

    // Start the gateway (its own thread + reconnect loop).
    auto client = client_.lock();
    if (client && client->HasToken())
    {
        gateway_ = std::make_shared<DiscordGateway>();
        gatewayThread_ = std::thread([this, client]() {
            gateway_->Run(client->Token(),
                          client->Intents(),
                          [this](std::string const& name, std::string const& data) {
                              DispatchEvent(name, data);
                          },
                          [this]() { return statusTextProvider_ ? statusTextProvider_() : std::string(); },
                          [this]() { return activityTypeProvider_ ? activityTypeProvider_() : std::string(); });
        });
    }

    DiscordOutMessage msg;
    while (running_)
    {
        if (!queue_.Pop(msg, 500))
            continue;

        if (msg.path == "ws")
        {
            if (gateway_)
                gateway_->Publish(msg.payload);
            continue;
        }

        HandleHttpMessage(msg);
    }

    LOG_INFO("modules.discord.net", "Discord REST worker thread stopped.");
}

void DiscordNetWorker::HandleHttpMessage(DiscordOutMessage& msg)
{
    std::string url;
    bool hasAuth = msg.needsAuth;
    bool isWebhook = msg.isWebhook;
    std::string token;
    bool logHttp = false;

    auto client = client_.lock();
    if (client)
    {
        token = client->Token();
        logHttp = client->LogHttp();
    }

    if (isWebhook)
        url = msg.path; // path holds the full webhook URL
    else
        url = std::string("https://") + DiscordChat::API_HOST + DiscordChat::API_VERSION + msg.path;

    std::string responseBody;
    long long responseCode = 0;
    int status = http_.Request(url, msg.method, msg.payload, token, hasAuth, isWebhook,
                               logHttp, &responseBody, &responseCode);

    if (status == 429)
    {
        // Bounded per-message retry (never stall the whole worker): sleep at
        // most 2s, retry once, then drop the message.
        long long backoff = 250;
        DiscordJson::Value parsed;
        if (DiscordJson::Parse(responseBody, parsed))
        {
            long long retryAfter = DiscordJson::GetInt(parsed, "retry_after");
            if (retryAfter > 0)
                backoff = std::min<long long>(retryAfter * 1000LL, 2000);
        }

        if (!msg.retried)
        {
            msg.retried = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
            queue_.Push(std::move(msg));
        }
        else
        {
            LOG_WARN("modules.discord.net", "Discord request still rate-limited after retry; dropping: {}", msg.path);
        }
        return;
    }

    if (msg.callback)
    {
        try { msg.callback(responseBody); }
        catch (std::exception const& e) { LOG_WARN("modules.discord.net", "Discord callback error: {}", e.what()); }
    }
}