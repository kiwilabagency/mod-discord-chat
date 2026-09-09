#include "DiscordGateway.h"
#include "DiscordJson.h"
#include "Log.h"

#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <openssl/ssl.h>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// Concrete websocket connection shared between the reader and writer loops.
struct DiscordGateway::GwConn
{
    net::io_context ioc;
    ssl::context sslCtx{ssl::context::tlsv12_client};
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws;
    bool open = false;
    std::mutex writeMutex;
};

namespace
{
    int ActivityTypeFromString(std::string const& type)
    {
        std::string lower;
        for (char c : type)
            lower += char(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "watching") return 3;
        if (lower == "listening") return 2;
        if (lower == "competing") return 5;
        return 0; // playing
    }

    std::string MakePresencePayload(std::string const& type, std::string const& text)
    {
        return "{\"op\":3,\"d\":{\"since\":0,\"activities\":[{\"name\":\"" + DiscordJson::Escape(text) +
               "\",\"type\":" + std::to_string(ActivityTypeFromString(type)) +
               "}],\"status\":\"online\",\"afk\":false}}";
    }

    std::string MakeIdentifyPayload(std::string const& token, uint32 intents)
    {
        std::string props = "{\"$os\":\"windows\",\"$browser\":\"AzerothCore-mod-discord-chat\",\"$device\":\"worldserver\"}";
        return "{\"op\":2,\"d\":{\"token\":\"" + DiscordJson::Escape(token) + "\",\"intents\":" +
               std::to_string(intents) + ",\"properties\":" + props +
               ",\"presence\":{\"activities\":[],\"status\":\"online\",\"afk\":false}}}";
    }
}

DiscordGateway::DiscordGateway() = default;
DiscordGateway::~DiscordGateway()
{
    Stop();
}

void DiscordGateway::Stop()
{
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(pubMutex_);
        wake_ = true;
    }
    pubCv_.notify_all();
}

void DiscordGateway::Publish(std::string const& payload)
{
    if (!running_.load())
        return;
    {
        std::lock_guard<std::mutex> lock(pubMutex_);
        publishQueue_.push(payload);
    }
    pubCv_.notify_one();
}

std::string DiscordGateway::MakePresencePayload() const
{
    std::string type = presenceTypeProvider_ ? presenceTypeProvider_() : "Playing";
    std::string text = statusTextProvider_ ? statusTextProvider_() : "";
    return ::MakePresencePayload(type, text);
}

void DiscordGateway::SendText(std::string const& payload)
{
    std::shared_ptr<GwConn> conn;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        conn = conn_;
    }
    if (!conn || !conn->open)
        return;
    std::lock_guard<std::mutex> lock(pubMutex_);
    publishQueue_.push(payload);
    pubCv_.notify_one();
}

void DiscordGateway::Run(std::string const& token, uint32 intents,
                         std::function<void(std::string const&, std::string const&)> eventHandler,
                         std::function<std::string()> statusTextProvider,
                         std::function<std::string()> presenceTypeProvider)
{
    token_ = token;
    intents_ = intents;
    eventHandler_ = std::move(eventHandler);
    statusTextProvider_ = std::move(statusTextProvider);
    presenceTypeProvider_ = std::move(presenceTypeProvider);

    running_.store(true);
    connected_.store(false);

    std::string host = "gateway.discord.gg";
    std::string port = "443";
    std::string path = "/?v=10&encoding=json";

    while (running_.load())
    {
        try
        {
            auto conn = std::make_shared<GwConn>();
            conn->sslCtx.set_default_verify_paths();
            conn->sslCtx.set_verify_mode(ssl::verify_none);

            tcp::resolver resolver(conn->ioc);
            auto results = resolver.resolve(host, port);
            conn->ws = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(conn->ioc, conn->sslCtx);
            beast::get_lowest_layer(*conn->ws).connect(results);
            if (!SSL_set_tlsext_host_name(conn->ws->next_layer().native_handle(), host.c_str()))
                throw std::runtime_error("SNI hostname failure");
            conn->ws->next_layer().handshake(ssl::stream_base::client);
            conn->ws->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
                req.set(boost::beast::http::field::user_agent, "AzerothCore-mod-discord-chat/1.0");
            }));
            conn->ws->handshake(host, path);
            conn->open = true;

            {
                std::lock_guard<std::mutex> lock(connMutex_);
                conn_ = conn;
            }

            // Writer thread drains the publish queue.
            std::thread writer([this, conn]() { WriteLoop(conn); });

            // Reader loop on this thread.
            ReadLoop(conn);

            {
                std::lock_guard<std::mutex> lock(connMutex_);
                conn_ = nullptr;
            }
            conn->open = false;
            connected_.store(false);
            wake_ = true;
            pubCv_.notify_all();
            if (writer.joinable())
                writer.join();
            if (conn->open)
            {
                try { conn->ws->close(websocket::close_code::normal); } catch (...) {}
            }

            // If Stop() was requested, exit; otherwise reconnect (RECONNECT op,
            // dropped connection, or INVALID_SESSION).
            if (!running_.load())
                break;
        }
        catch (std::exception const& e)
        {
            connected_.store(false);
            LOG_WARN("modules.discord.gateway", "Gateway connection error: {}. Retrying.", e.what());
            // 5s retry cadence with a handful of short backoff ticks.
            for (int i = 0; i < 20 && running_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    LOG_INFO("modules.discord.gateway", "Gateway stopped.");
}

void DiscordGateway::ReadLoop(std::shared_ptr<GwConn> conn)
{
    beast::flat_buffer buffer;
    std::chrono::steady_clock::time_point lastHeartbeat = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastPresence = std::chrono::steady_clock::now();
    int heartbeatIntervalMs = 41250;

    while (running_.load())
    {
        // Short read timeout so Stop() can interrupt the blocking read.
        beast::error_code ec;
        beast::get_lowest_layer(*conn->ws).expires_after(std::chrono::milliseconds(500));
        std::string payload;
        conn->ws->read(buffer, ec);
        if (ec)
        {
            if (ec != beast::error::timeout && ec != net::error::would_block)
                break; // connection broken
        }
        else if (conn->ws->got_text())
        {
            payload.assign(beast::buffers_to_string(buffer.data()));
            buffer.consume(buffer.size());

            DiscordJson::Value envelope;
            if (DiscordJson::Parse(payload, envelope))
            {
                long long op = DiscordJson::GetInt(envelope, "op");
                std::string eventName = DiscordJson::GetString(envelope, "t");

                switch (op)
                {
                    case 10: // HELLO
                    {
                        DiscordJson::Value const* d = envelope.get("d");
                        if (d)
                            heartbeatIntervalMs = (int)DiscordJson::GetInt(*d, "heartbeat_interval");
                        if (heartbeatIntervalMs < 1000)
                            heartbeatIntervalMs = 1000;
                        SendText(MakeIdentifyPayload(token_, intents_));
                        lastHeartbeat = std::chrono::steady_clock::now();
                        break;
                    }
                    case 1: // HEARTBEAT request from server
                        SendText("{\"op\":1,\"d\":null}");
                        lastHeartbeat = std::chrono::steady_clock::now();
                        break;
                    case 7: // RECONNECT
                        return;
                    case 9: // INVALID_SESSION (resumable flag in d)
                        SendText(MakeIdentifyPayload(token_, intents_));
                        break;
                    case 11: // HEARTBEAT_ACK
                        break;
                    case 0: // DISPATCH
                        if (eventName == "READY")
                        {
                            connected_.store(true);
                            // Show the bot presence immediately, before the first
                            // periodic refresh ticks.
                            SendText(MakePresencePayload());
                            lastPresence = std::chrono::steady_clock::now();
                        }
                        if (eventHandler_ && !eventName.empty())
                        {
                            try { eventHandler_(eventName, payload); }
                            catch (std::exception const& e)
                            {
                                LOG_WARN("modules.discord.gateway", "Event handler error: {}", e.what());
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        else
        {
            // Binary frame (e.g. compressed); not used by Discord's gateway.
            buffer.consume(buffer.size());
        }

        auto now = std::chrono::steady_clock::now();
        // Heartbeat.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHeartbeat).count() >= heartbeatIntervalMs)
        {
            SendText("{\"op\":1,\"d\":null}");
            lastHeartbeat = now;
        }
        // Presence refresh (throttled, only when the text changes is handled by the caller).
        if (statusTextProvider_ &&
            std::chrono::duration_cast<std::chrono::seconds>(now - lastPresence).count() >= 15)
        {
            SendText(MakePresencePayload());
            lastPresence = now;
        }
    }
}

void DiscordGateway::WriteLoop(std::shared_ptr<GwConn> conn)
{
    while (running_.load() && conn->open)
    {
        std::string payload;
        {
            std::unique_lock<std::mutex> lock(pubMutex_);
            pubCv_.wait_for(lock, std::chrono::milliseconds(300), [this] { return !publishQueue_.empty() || wake_; });
            if (!publishQueue_.empty())
            {
                payload = publishQueue_.front();
                publishQueue_.pop();
            }
        }
        if (!payload.empty() && conn->open)
        {
            std::lock_guard<std::mutex> wlock(conn->writeMutex);
            beast::error_code ec;
            conn->ws->text(true);
            conn->ws->write(net::buffer(payload), ec);
            if (ec)
                break;
        }
    }
}