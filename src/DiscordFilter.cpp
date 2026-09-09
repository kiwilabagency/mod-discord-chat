#include "DiscordFilter.h"
#include "DiscordConfig.h"
#include "DiscordMgr.h"
#include "Log.h"

#include <cctype>

namespace
{
    // Starter list used when Discord.Filter.Words is empty. Owners should extend
    // this with their own list in the configuration. Only severe slurs/hate
    // speech are included by default; mild profanity is intentionally allowed.
    char const* kDefaultFilterWords =
        "nigger,nigga,kike,spic,spick,wetback,chink,gook,faggot,fag,tranny,jigaboo,towelhead,beaner";

    std::string Trim(std::string s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    std::vector<std::string> SplitList(std::string const& raw)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= raw.size())
        {
            size_t comma = raw.find(',', start);
            std::string part = comma == std::string::npos ? raw.substr(start) : raw.substr(start, comma - start);
            part = Trim(part);
            if (!part.empty())
                out.push_back(part);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return out;
    }
}

void DiscordFilter::Load()
{
    DiscordConfig const* cfg = sDiscordMgr->Config();
    enabled_ = cfg && cfg->FilterEnable;
    if (!cfg)
        return;

    applyToGame_ = cfg->FilterDiscordToGame;
    applyToDiscord_ = cfg->FilterGameToDiscord;
    censorMode_ = cfg->FilterMode == "censor";

    banned_.clear();
    std::string words = cfg->FilterWords;
    if (words.empty())
        words = kDefaultFilterWords;
    for (std::string const& w : SplitList(words))
        banned_.push_back(NormalizeWord(w));

    if (enabled_)
        LOG_INFO("modules.discord.filter", "Chat Filter: Enabled ({}, {} words)",
                 censorMode_ ? "censor" : "block", banned_.size());
}

// Lowercase + light leetspeak normalization of a single word.
std::string DiscordFilter::NormalizeWord(std::string word)
{
    std::string out;
    out.reserve(word.size());
    for (char c : word)
    {
        c = char(std::tolower(static_cast<unsigned char>(c)));
        switch (c)
        {
            case '0': c = 'o'; break;
            case '1': c = 'i'; break;
            case '2': c = 'z'; break;
            case '3': c = 'e'; break;
            case '4': c = 'a'; break;
            case '5': c = 's'; break;
            case '7': c = 't'; break;
            case '8': c = 'b'; break;
            case '9': c = 'g'; break;
            case '@': c = 'a'; break;
            case '$': c = 's'; break;
            case '!': c = 'i'; break;
            default: break;
        }
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += c;
    }
    return out;
}

// Splits text into raw words (alphanumeric runs) on the original text, so word
// boundaries are preserved before normalization.
std::vector<std::string> DiscordFilter::SplitWords(std::string const& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : text)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
            cur += c;
        else if (!cur.empty())
        {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// Replaces URL regions (http://, https://, www., ftp://) with spaces so that
// link paths are never blocked or censored by the word filter.
std::string DiscordFilter::MaskUrls(std::string const& text)
{
    std::string out = text;
    for (size_t i = 0; i < out.size();)
    {
        bool urlStart =
            out.compare(i, 7, "http://") == 0 || out.compare(i, 8, "https://") == 0 ||
            out.compare(i, 6, "ftp://") == 0 || out.compare(i, 4, "www.") == 0;
        if (!urlStart)
        {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < out.size() && !std::isspace(static_cast<unsigned char>(out[end])))
            ++end;
        for (size_t j = i; j < end; ++j)
            out[j] = ' ';
        i = end;
    }
    return out;
}

bool DiscordFilter::IsBannedWord(std::string const& rawWord) const
{
    std::string norm = NormalizeWord(rawWord);
    for (std::string const& banned : banned_)
        if (norm == banned)
            return true;
    return false;
}

bool DiscordFilter::HasBannedWord(std::string const& text) const
{
    if (!enabled_ || banned_.empty())
        return false;
    std::string masked = MaskUrls(text);
    for (std::string const& word : SplitWords(masked))
        if (IsBannedWord(word))
            return true;
    return false;
}

std::string DiscordFilter::Censor(std::string const& text) const
{
    if (!enabled_ || banned_.empty())
        return text;

    std::string masked = MaskUrls(text);
    std::string out;
    out.reserve(text.size());
    std::string cur;
    size_t wordStart = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            if (cur.empty())
                wordStart = i;
            cur += c;
        }
        else
        {
            if (!cur.empty())
            {
                // Skip words that belong to a URL (masked region is spaces).
                bool inUrl = std::isalnum(static_cast<unsigned char>(masked[wordStart])) == false;
                out += (inUrl || !IsBannedWord(cur)) ? cur : std::string("***");
                cur.clear();
            }
            out += c;
        }
    }
    if (!cur.empty())
    {
        bool inUrl = std::isalnum(static_cast<unsigned char>(masked[wordStart])) == false;
        out += (inUrl || !IsBannedWord(cur)) ? cur : std::string("***");
    }
    return out;
}

std::string DiscordFilter::Handle(std::string const& text) const
{
    if (!enabled_)
        return text;
    if (censorMode_)
        return Censor(text);
    return HasBannedWord(text) ? std::string() : text;
}