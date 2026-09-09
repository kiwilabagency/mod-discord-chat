#ifndef DISCORD_HTTP_H_
#define DISCORD_HTTP_H_

#include "DiscordModule.h"

// Performs HTTPS REST requests to the Discord API and to webhook URLs, with a
// global Discord rate-limit guard (429 handling with retry-after backoff) and
// per-channel webhook throttling. Called exclusively from the worker thread.
class DiscordHttp
{
public:
    DiscordHttp();
    ~DiscordHttp();

    // Execute one request synchronously on the calling (worker) thread.
    // Returns HTTP status code (200..299 success). On network failure returns 0.
    // body and response are optional out-params.
    int Request(std::string const& url, std::string const& method, std::string const& payload,
                std::string const& authToken, bool hasAuth, bool isWebhook, bool logHttp,
                std::string* responseBody, long long* responseCode);

    // Serialize access to asio + OpenSSL context.
    std::mutex& Mutex() { return mutex_; }

private:
    std::mutex mutex_;
    bool InitSslOnce(std::string& err);
    void* ctx_ = nullptr; // SSL_CTX*
};

#endif // DISCORD_HTTP_H_