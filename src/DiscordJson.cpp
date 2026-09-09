#include "DiscordJson.h"
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <stdexcept>

namespace DiscordJson
{
    std::string Escape(std::string_view value)
    {
        std::string out;
        out.reserve(value.size() + 8);
        for (char c : value)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        out += "\\u" "00";
                    else
                        out += c;
                    break;
            }
        }
        return out;
    }

    void Writer::Open()
    {
        out = "{";
        first = true;
    }

    void Writer::Close()
    {
        out += "}";
    }

    void Writer::Key(std::string const& key)
    {
        if (!first)
            out += ",";
        first = false;
        out += "\"";
        out += Escape(key);
        out += "\":";
    }

    void Writer::Field(std::string const& key, std::string const& value)
    {
        Key(key);
        out += "\"";
        out += Escape(value);
        out += "\"";
    }

    void Writer::Field(std::string const& key, std::string_view value)
    {
        Field(key, std::string(value));
    }

    void Writer::Field(std::string const& key, char const* value)
    {
        Field(key, std::string(value ? value : ""));
    }

    void Writer::Field(std::string const& key, long long value)
    {
        Key(key);
        out += std::to_string(value);
    }

    void Writer::Field(std::string const& key, bool value)
    {
        Key(key);
        out += value ? "true" : "false";
    }

    void Writer::Field(std::string const& key, double value)
    {
        Key(key);
        out += std::to_string(value);
    }

    void Writer::Raw(std::string const& key, std::string const& json)
    {
        Key(key);
        out += json;
    }

    // ---- Reader ------------------------------------------------------------
    namespace
    {
        struct Parser
        {
            std::string_view src;
            size_t pos = 0;

            void SkipWs()
            {
                while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos])))
                    ++pos;
            }

            bool Consume(char c)
            {
                SkipWs();
                if (pos < src.size() && src[pos] == c)
                {
                    ++pos;
                    return true;
                }
                return false;
            }

            bool ParseValue(Value& out)
            {
                SkipWs();
                if (pos >= src.size())
                    return false;
                char c = src[pos];
                if (c == '{') return ParseObject(out);
                if (c == '[') return ParseArray(out);
                if (c == '"') { std::string s; if (!ParseString(s)) return false; out.kind = Value::String; out.s = s; return true; }
                if (c == 't') return ParseLiteral("true", Value::Bool, true, out);
                if (c == 'f') return ParseLiteral("false", Value::Bool, false, out);
                if (c == 'n') return ParseLiteral("null", Value::Null, false, out);
                if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber(out);
                return false;
            }

            bool ParseLiteral(char const* lit, Value::Kind kind, bool b, Value& out)
            {
                size_t len = std::string(lit).size();
                if (src.substr(pos, len) != std::string(lit))
                    return false;
                pos += len;
                out.kind = kind;
                out.b = b;
                return true;
            }

            bool ParseNumber(Value& out)
            {
                size_t start = pos;
                bool isDouble = false;
                while (pos < src.size())
                {
                    char c = src[pos];
                    if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
                    {
                        if (c == '.' || c == 'e' || c == 'E')
                            isDouble = true;
                        ++pos;
                    }
                    else
                        break;
                }
                std::string tok(src.substr(start, pos - start));
                if (tok.empty())
                    return false;
                try
                {
                    if (isDouble)
                    {
                        out.kind = Value::Double;
                        out.d = std::stod(tok);
                    }
                    else
                    {
                        out.kind = Value::Int;
                        out.i = std::stoll(tok);
                    }
                }
                catch (...) { return false; }
                return true;
            }

            bool ParseString(std::string& out)
            {
                if (!Consume('"'))
                    return false;
                out.clear();
                while (pos < src.size())
                {
                    char c = src[pos++];
                    if (c == '"')
                        return true;
                    if (c == '\\')
                    {
                        if (pos >= src.size()) return false;
                        char e = src[pos++];
                        switch (e)
                        {
                            case '"': out += '"'; break;
                            case '\\': out += '\\'; break;
                            case '/': out += '/'; break;
                            case 'n': out += '\n'; break;
                            case 'r': out += '\r'; break;
                            case 't': out += '\t'; break;
                            case 'b': out += '\b'; break;
                            case 'f': out += '\f'; break;
                            case 'u':
                            {
                                if (pos + 4 > src.size()) return false;
                                unsigned cp = 0;
                                for (int i = 0; i < 4; ++i)
                                {
                                    char h = src[pos++];
                                    cp <<= 4;
                                    if (h >= '0' && h <= '9') cp |= h - '0';
                                    else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                                    else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                                    else return false;
                                }
                                // Encode surrogate pair for code points above BMP.
                                if (cp >= 0xD800 && cp <= 0xDBFF)
                                {
                                    if (pos + 6 > src.size()) return false;
                                    if (src[pos] != '\\' || src[pos + 1] != 'u') return false;
                                    unsigned lo = 0;
                                    for (int i = 0; i < 4; ++i)
                                    {
                                        char h = src[pos + 2 + i];
                                        lo <<= 4;
                                        if (h >= '0' && h <= '9') lo |= h - '0';
                                        else if (h >= 'a' && h <= 'f') lo |= h - 'a' + 10;
                                        else if (h >= 'A' && h <= 'F') lo |= h - 'A' + 10;
                                        else return false;
                                    }
                                    pos += 6;
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                }
                                // UTF-8 encode
                                if (cp < 0x80) out += char(cp);
                                else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                                else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                                else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                                break;
                            }
                            default: out += e; break;
                        }
                    }
                    else
                        out += c;
                }
                return false;
            }

            bool ParseArray(Value& out)
            {
                out.kind = Value::Array;
                if (!Consume('['))
                    return false;
                SkipWs();
                if (Consume(']'))
                    return true;
                for (;;)
                {
                    Value item;
                    if (!ParseValue(item))
                        return false;
                    out.arr.push_back(std::move(item));
                    SkipWs();
                    if (Consume(']'))
                        return true;
                    if (!Consume(','))
                        return false;
                }
            }

            bool ParseObject(Value& out)
            {
                out.kind = Value::Object;
                if (!Consume('{'))
                    return false;
                SkipWs();
                if (Consume('}'))
                    return true;
                for (;;)
                {
                    SkipWs();
                    std::string key;
                    if (!ParseString(key))
                        return false;
                    if (!Consume(':'))
                        return false;
                    Value item;
                    if (!ParseValue(item))
                        return false;
                    out.obj.emplace_back(std::move(key), std::move(item));
                    SkipWs();
                    if (Consume('}'))
                        return true;
                    if (!Consume(','))
                        return false;
                }
            }
        };
    }

    Value const* Value::get(std::string const& key) const
    {
        if (kind != Object)
            return nullptr;
        for (auto const& kv : obj)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }

    std::vector<Value> const& Value::asArray() const
    {
        static std::vector<Value> const empty;
        return (kind == Array) ? arr : empty;
    }

    bool Parse(std::string_view json, Value& out)
    {
        Parser p;
        p.src = json;
        if (!p.ParseValue(out))
        {
            out = Value();
            return false;
        }
        p.SkipWs();
        return p.pos >= p.src.size();
    }

    std::string GetString(Value const& v, std::string const& key)
    {
        Value const* f = v.get(key);
        return (f && f->kind == Value::String) ? f->s : std::string();
    }

    long long GetInt(Value const& v, std::string const& key)
    {
        Value const* f = v.get(key);
        if (!f)
            return 0;
        if (f->kind == Value::Int) return f->i;
        if (f->kind == Value::Double) return (long long)f->d;
        if (f->kind == Value::String) { try { return std::stoll(f->s); } catch (...) { return 0; } }
        return 0;
    }

    bool GetBool(Value const& v, std::string const& key)
    {
        Value const* f = v.get(key);
        return f && f->kind == Value::Bool ? f->b : false;
    }
}