#ifndef DISCORD_JSON_H_
#define DISCORD_JSON_H_

#include "DiscordModule.h"

namespace DiscordJson
{
    // Minimal JSON object builder (field ordering preserved) and a tiny reader
    // for the subset of the Discord API we consume. We deliberately avoid a
    // heavy third-party JSON dependency.

    // ---- Writer ------------------------------------------------------------
    std::string Escape(std::string_view value);

    struct Writer
    {
        std::string out;
        bool first = true;

        void Open();
        void Close();
        void Key(std::string const& key);
        void Field(std::string const& key, std::string const& value);  // string
        void Field(std::string const& key, std::string_view value);     // string
        void Field(std::string const& key, char const* value);          // string
        void Field(std::string const& key, long long value);
        void Field(std::string const& key, bool value);
        void Field(std::string const& key, double value);
        void Raw(std::string const& key, std::string const& json);      // prebuilt JSON value
    };

    // ---- Reader ------------------------------------------------------------
    struct Value
    {
        enum Kind { Null, Bool, Int, Double, String, Array, Object };
        Kind kind = Null;
        bool b = false;
        long long i = 0;
        double d = 0;
        std::string s;
        std::vector<std::pair<std::string, Value>> obj;
        std::vector<Value> arr;

        Value() : kind(Null) {}
        bool isNull() const { return kind == Null; }
        Value const* get(std::string const& key) const;
        std::vector<Value> const& asArray() const;
    };

    // Parse returns true on success; on failure result is left Null.
    bool Parse(std::string_view json, Value& out);

    // Convenience: read a string/uint field.
    std::string GetString(Value const& v, std::string const& key);
    long long GetInt(Value const& v, std::string const& key);
    bool GetBool(Value const& v, std::string const& key);
}

#endif // DISCORD_JSON_H_