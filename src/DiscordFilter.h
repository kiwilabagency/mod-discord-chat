#ifndef DISCORD_FILTER_H_
#define DISCORD_FILTER_H_

#include "Define.h"
#include <string>
#include <vector>

// Simple chat filter for the Global Chat bridge. Blocks (or censors) messages
// that contain banned words (slurs, hate speech) while allowing mild profanity.
//
// Matching is case-insensitive and applies a light leetspeak normalization
// (e.g. "h1tler" -> "hitler") to catch obvious evasions. Whole words are
// matched, so a banned word never matches inside an innocent longer word.
//
// The banned word list is loaded from Discord.Filter.Words (comma separated).
class DiscordFilter
{
public:
    void Load();

    bool Enabled() const { return enabled_; }
    bool AppliesToGame() const { return enabled_ && applyToGame_; }
    bool AppliesToDiscord() const { return enabled_ && applyToDiscord_; }

    // True if the text contains a banned word.
    bool HasBannedWord(std::string const& text) const;

    // Replaces banned words with "***" (censor mode).
    std::string Censor(std::string const& text) const;

    // Returns "" when the message should be blocked, or the (possibly censored)
    // text that is allowed through.
    std::string Handle(std::string const& text) const;

private:
    static std::string NormalizeWord(std::string word);
    static std::vector<std::string> SplitWords(std::string const& text);
    static std::string MaskUrls(std::string const& text);
    bool IsBannedWord(std::string const& rawWord) const;

    bool enabled_ = false;
    bool applyToGame_ = true;
    bool applyToDiscord_ = true;
    bool censorMode_ = false;             // false = block, true = censor
    std::vector<std::string> banned_;     // normalized banned words
};

#endif // DISCORD_FILTER_H_