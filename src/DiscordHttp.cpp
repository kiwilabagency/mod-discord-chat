#include "DiscordHttp.h"
#include "DiscordJson.h"
#include "Log.h"

#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <chrono>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace
{
    struct UrlParts
    {
        std::string scheme;
        std::string host;
        std::string port;
        std::string target;
    };

    UrlParts SplitUrl(std::string const& url)
    {
        UrlParts p;
        std::string rest = url;
        size_t schemePos = rest.find("://");
        if (schemePos != std::string::npos)
        {
            p.scheme = rest.substr(0, schemePos);
            rest = rest.substr(schemePos + 3);
        }
        else
            p.scheme = "https";

        size_t slash = rest.find('/');
        std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
        p.target = slash == std::string::npos ? "/" : rest.substr(slash);

        size_t colon = hostPort.find(':');
        if (colon != std::string::npos)
        {
            p.host = hostPort.substr(0, colon);
            p.port = hostPort.substr(colon + 1);
        }
        else
        {
            p.host = hostPort;
            p.port = (p.scheme == "http") ? "80" : "443";
        }
        return p;
    }
}

DiscordHttp::DiscordHttp() = default;
DiscordHttp::~DiscordHttp() = default;

bool DiscordHttp::InitSslOnce(std::string& err)
{
    if (ctx_)
        return true;
    // Create a fresh SSL_CTX. This uses the OpenSSL helper types directly.
    // (Simplified: we rely on Beast's stream with a raw SSL_CTX.)
    // Actually initialize lazily inside Request via Boost's ssl::context for clarity.
    return true;
}

int DiscordHttp::Request(std::string const& url, std::string const& method, std::string const& payload,
                         std::string const& authToken, bool hasAuth, bool isWebhook, bool logHttp,
                         std::string* responseBody, long long* responseCode)
{
    std::lock_guard<std::mutex> guard(mutex_);

    UrlParts parts = SplitUrl(url);

    try
    {
        net::io_context ioc;
        ssl::context sslCtx(ssl::context::tlsv12_client);
        sslCtx.set_default_verify_paths();
        sslCtx.set_verify_mode(ssl::verify_none); // accept all CA chains (see note)

        // Bound every network operation so a stalled request can never block the
        // worker thread (and with it, all queued Discord messages) indefinitely.
        auto const withTimeout = [](beast::tcp_stream& s) { s.expires_after(std::chrono::seconds(10)); };

        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(parts.host, parts.port);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, sslCtx);
        auto& lowest = beast::get_lowest_layer(stream);
        withTimeout(lowest);
        lowest.connect(results);
        withTimeout(lowest);
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req;
        req.method(http::string_to_verb(method));
        req.target(parts.target);
        req.version(11);
        req.set(http::field::host, parts.host);
        req.set(http::field::user_agent, "AzerothCore-mod-discord-chat/1.0");
        req.set(http::field::content_type, "application/json");
        if (hasAuth && !authToken.empty())
            req.set(http::field::authorization, "Bot " + authToken);
        req.body() = payload;
        req.prepare_payload();

        withTimeout(lowest);
        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        withTimeout(lowest);
        http::read(stream, buffer, res);
        lowest.expires_never();

        int status = res.result_int();
        if (responseCode)
            *responseCode = status;
        if (responseBody)
            *responseBody = res.body();

        // Fast teardown: do not wait for a TLS close_notify round-trip. Some
        // Discord endpoints keep keep-alive connections open, and a blocking
        // SSL shutdown would stall the worker thread (delaying all messages).
        beast::error_code ec;
        lowest.socket().shutdown(net::socket_base::shutdown_both, ec);
        lowest.socket().close(ec);

        if (logHttp)
            LOG_DEBUG("modules.discord.http", "Discord HTTP {} {} -> {}", method, parts.target, status);

        return status;
    }
    catch (std::exception const& e)
    {
        if (logHttp)
            LOG_WARN("modules.discord.http", "Discord HTTP request failed ({} {}): {}", method, parts.target, e.what());
        return 0;
    }
}