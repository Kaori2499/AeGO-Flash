#include "MotionJson.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace l2dae {
namespace {
constexpr std::size_t kMaxBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxNodes = 4 * 1024 * 1024;
constexpr std::size_t kMaxCurves = 16384;
constexpr std::size_t kMaxSegments = 1024 * 1024;
constexpr std::size_t kMaxPoints = 3 * kMaxSegments + kMaxCurves;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("Motion JSON: " + message);
}

struct Value {
    enum Kind { Null, Boolean, Number, String, Array, Object, Infinite } kind = Null;
    double number = 0;
    bool boolean = false;
    std::string text;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;
    Value* find(const char* key) {
        for (auto& entry : object) if (entry.first == key) return &entry.second;
        return nullptr;
    }
};

class Parser {
    const std::string input_;
    std::size_t pos_ = 0;
    std::size_t nodes_ = 0;
    [[noreturn]] void invalid() const { fail("malformed JSON near byte " + std::to_string(pos_) + "."); }
    void whitespace() {
        while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t' ||
            input_[pos_] == '\r' || input_[pos_] == '\n')) ++pos_;
    }
    bool take(char expected) {
        whitespace();
        if (pos_ == input_.size() || input_[pos_] != expected) return false;
        ++pos_; return true;
    }
    unsigned hex4() {
        if (pos_ + 4 > input_.size()) invalid();
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            const int digit = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) invalid();
            code = code * 16 + static_cast<unsigned>(digit);
        }
        return code;
    }
    static void utf8(std::string& output, unsigned code) {
        if (code < 0x80) output += static_cast<char>(code);
        else if (code < 0x800) {
            output += static_cast<char>(0xc0 | (code >> 6));
            output += static_cast<char>(0x80 | (code & 0x3f));
        } else if (code < 0x10000) {
            output += static_cast<char>(0xe0 | (code >> 12));
            output += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            output += static_cast<char>(0xf0 | (code >> 18));
            output += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            output += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            output += static_cast<char>(0x80 | (code & 0x3f));
        }
    }
    std::string string() {
        if (!take('"')) invalid();
        std::string result;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c < 32) invalid();
            if (c == '"') return result;
            if (c != '\\') { result += static_cast<char>(c); continue; }
            if (pos_ == input_.size()) invalid();
            switch (input_[pos_++]) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                unsigned code = hex4();
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (pos_ + 2 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') invalid();
                    pos_ += 2;
                    const unsigned low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) invalid();
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) invalid();
                if (code < 32 && code != '\b' && code != '\f' && code != '\n' && code != '\r' && code != '\t')
                    fail("unsupported string control character.");
                utf8(result, code); break;
            }
            default: invalid();
            }
        }
        invalid();
    }
    Value number() {
        Value result; result.kind = Value::Number;
        const auto start = pos_;
        if (input_[pos_] == '-') ++pos_;
        const auto digits = [&] {
            const auto begin = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
            return pos_ > begin;
        };
        if (pos_ == input_.size()) invalid();
        if (input_[pos_] == '0') ++pos_;
        else if (input_[pos_] < '1' || input_[pos_] > '9' || !digits()) invalid();
        if (pos_ < input_.size() && input_[pos_] == '.') { ++pos_; if (!digits()) invalid(); }
        bool exponent = false;
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            exponent = true; ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (!digits()) invalid();
        }
        const auto parsed = std::from_chars(input_.data() + start, input_.data() + pos_, result.number);
        if (parsed.ec != std::errc{} || parsed.ptr != input_.data() + pos_ || !std::isfinite(result.number) ||
            !std::isfinite(static_cast<float>(result.number)) ||
            (result.number != 0 && static_cast<float>(result.number) == 0))
            fail("numbers must be finite and representable as 32-bit values.");
        if (exponent) {
            char decimal[512];
            const auto formatted = std::to_chars(decimal, decimal + sizeof decimal,
                static_cast<float>(result.number), std::chars_format::fixed);
            if (formatted.ec != std::errc{}) invalid();
            result.text.assign(decimal, formatted.ptr);
        } else result.text.assign(input_, start, pos_ - start);
        return result;
    }
    Value value(int depth) {
        whitespace();
        if (depth > 64 || ++nodes_ > kMaxNodes || pos_ == input_.size()) invalid();
        Value result;
        const char current = input_[pos_];
        if (current == '"') { result.kind = Value::String; result.text = string(); return result; }
        if (current == '{') {
            take('{'); result.kind = Value::Object;
            if (take('}')) return result;
            std::set<std::string> keys;
            do {
                auto key = string();
                if (!keys.insert(key).second) fail("duplicate object key: " + key + ".");
                if (!take(':')) invalid();
                result.object.emplace_back(std::move(key), value(depth + 1));
                if (take('}')) return result;
            } while (take(','));
            invalid();
        }
        if (current == '[') {
            take('['); result.kind = Value::Array;
            if (take(']')) return result;
            do { result.array.push_back(value(depth + 1)); if (take(']')) return result; } while (take(','));
            invalid();
        }
        for (const auto* literal : {"true", "false", "null", "-Infinity", "+Infinity", "Infinity"}) {
            const std::string token(literal);
            if (input_.compare(pos_, token.size(), token) != 0) continue;
            pos_ += token.size();
            if (token == "true" || token == "false") { result.kind = Value::Boolean; result.boolean = token == "true"; }
            else if (token != "null") result.kind = Value::Infinite;
            return result;
        }
        if (current == '-' || (current >= '0' && current <= '9')) return number();
        invalid();
    }
public:
    explicit Parser(const std::vector<unsigned char>& input) : input_(input.begin(), input.end()) {}
    Value parse() {
        if (input_.size() >= 3 && input_.compare(0, 3, "\xef\xbb\xbf") == 0) pos_ = 3;
        auto result = value(0); whitespace();
        if (pos_ != input_.size()) invalid();
        return result;
    }
};

Value& member(Value& object, const char* name, Value::Kind kind) {
    auto* value = object.find(name);
    if (object.kind != Value::Object || !value || value->kind != kind)
        fail(std::string("missing or invalid ") + name + ".");
    return *value;
}
std::size_t count(Value& meta, const char* name, std::size_t maximum) {
    const double n = member(meta, name, Value::Number).number;
    if (n < 0 || n != std::floor(n) || n > static_cast<double>(maximum))
        fail(std::string("invalid ") + name + ".");
    return static_cast<std::size_t>(n);
}
void optional(Value& object, const char* name, Value::Kind kind) {
    const auto* value = object.find(name);
    if (value && value->kind != kind) fail(std::string("invalid ") + name + ".");
}
Value integer(std::size_t n) {
    Value v; v.kind = Value::Number; v.number = static_cast<double>(n); v.text = std::to_string(n); return v;
}
void quote(std::string& output, const std::string& text) {
    output += '"';
    for (const char c : text) {
        switch (c) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output += c; break;
        }
    }
    output += '"';
}
void serialize(const Value& value, std::string& output) {
    switch (value.kind) {
    case Value::Null: output += "null"; break;
    case Value::Boolean: output += value.boolean ? "true" : "false"; break;
    case Value::Number: output += value.text; break;
    case Value::String: quote(output, value.text); break;
    case Value::Infinite: fail("Infinity is supported only for a stepped incoming Bezier tangent; invalid value elsewhere.");
    case Value::Array:
        output += "[\n";
        for (std::size_t i = 0; i < value.array.size(); ++i) {
            if (i) output += ",\n";
            serialize(value.array[i], output);
        }
        output += ']'; break;
    case Value::Object:
        output += "{\n";
        for (std::size_t i = 0; i < value.object.size(); ++i) {
            if (i) output += ",\n";
            quote(output, value.object[i].first); output += ":\n";
            serialize(value.object[i].second, output);
        }
        output += '}'; break;
    }
    output += '\n'; // Cubism's numeric parser requires a comma or newline.
    if (output.size() > 2 * kMaxBytes) fail("normalized file is too large.");
}
}

std::string terminateCubismNumbers(std::string_view text) {
    std::string output;
    output.reserve(text.size() + text.size() / 32);
    std::string held;
    bool inString = false, escape = false, lastDigit = false;
    const auto flush = [&] {
        output.append(held);
        held.clear();
    };
    for (const unsigned char value : text) {
        const char ch = static_cast<char>(value);
        if (inString) {
            flush();
            output += ch;
            if (escape) escape = false;
            else if (ch == '\\') escape = true;
            else if (ch == '"') {
                inString = false;
                lastDigit = false;
            }
            continue;
        }
        if (ch == '"') {
            flush();
            output += ch;
            inString = true;
            lastDigit = false;
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            held += ch;
            continue;
        }
        if ((ch == '}' || ch == ']') && lastDigit) output += '\n';
        flush();
        output += ch;
        lastDigit = ch >= '0' && ch <= '9';
    }
    flush();
    return output;
}

MotionJsonResult normalizeMotionJson(const std::vector<unsigned char>& input) {
    if (input.empty() || input.size() > kMaxBytes) fail("file must be nonempty and at most 64 MiB.");
    auto root = Parser(input).parse();
    if (member(root, "Version", Value::Number).number != 3) fail("Version must be 3.");
    auto& meta = member(root, "Meta", Value::Object);
    const double duration = member(meta, "Duration", Value::Number).number;
    if (duration <= 0 || duration > 86400) fail("Duration must be greater than zero and at most 86400 seconds.");
    if (member(meta, "Fps", Value::Number).number <= 0) fail("Fps must be greater than zero.");
    optional(meta, "Loop", Value::Boolean);
    optional(meta, "AreBeziersRestricted", Value::Boolean);
    optional(meta, "FadeInTime", Value::Number);
    optional(meta, "FadeOutTime", Value::Number);
    const auto expectedCurves = count(meta, "CurveCount", kMaxCurves);
    const auto expectedSegments = count(meta, "TotalSegmentCount", kMaxSegments);
    const auto expectedPoints = count(meta, "TotalPointCount", kMaxPoints);
    auto& curves = member(root, "Curves", Value::Array).array;
    if (curves.size() != expectedCurves) fail("CurveCount does not match the curve array.");
    std::size_t segmentCount = 0, pointCount = 0, repaired = 0;
    for (std::size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
        auto& curve = curves[curveIndex];
        const auto& target = member(curve, "Target", Value::String).text;
        if (target != "Parameter" && target != "Model" && target != "PartOpacity") fail("unsupported curve Target.");
        const auto& id = member(curve, "Id", Value::String).text;
        if (id.empty() || id.size() > 1024) fail("curve Id must contain 1 to 1024 UTF-8 bytes.");
        const std::string context = "curve " + std::to_string(curveIndex) + " (" + id + "): ";
        optional(curve, "FadeInTime", Value::Number);
        optional(curve, "FadeOutTime", Value::Number);
        auto& segments = member(curve, "Segments", Value::Array).array;
        if (segments.size() < 5) fail(context + "Segments is truncated or contains no segment.");
        if (segments[0].kind != Value::Number || segments[1].kind != Value::Number || segments[0].number < 0)
            fail(context + "initial time and value must be finite, with nonnegative time.");
        double previousTime = segments[0].number, previousValue = segments[1].number;
        std::vector<Value> normalized;
        normalized.reserve(segments.size());
        normalized.push_back(segments[0]); normalized.push_back(segments[1]);
        ++pointCount;
        for (std::size_t i = 2; i < segments.size();) {
            if (segments[i].kind != Value::Number || segments[i].number < 0 || segments[i].number > 3 ||
                segments[i].number != std::floor(segments[i].number))
                fail(context + "unknown segment type; expected 0, 1, 2, or 3.");
            const int type = static_cast<int>(segments[i].number);
            const std::size_t width = type == 1 ? 7 : 3;
            if (width > segments.size() - i) fail(context + "truncated segment.");
            for (std::size_t j = 1; j < width; ++j) {
                if (type == 1 && j == 4 && segments[i + j].kind == Value::Infinite) continue;
                if (segments[i + j].kind != Value::Number) fail(context + "segment times and values must be finite numbers.");
            }
            const double endTime = segments[i + width - 2].number;
            const double endValue = segments[i + width - 1].number;
            if (static_cast<float>(endTime) <= static_cast<float>(previousTime))
                fail(context + "segment endpoint times must increase at 32-bit precision.");
            if (type == 1 && segments[i + 4].kind == Value::Infinite) {
                // Decompiled stepped curves can encode an infinite incoming
                // tangent. Restrict compatibility to this observed pattern;
                // retaining a huge value or replacing it with zero distorts it.
                if (segments[i + 2].number != previousValue)
                    fail(context + "unsupported infinite Bezier control value; first control value must equal the starting keyframe.");
                const double t1 = segments[i + 1].number, t2 = segments[i + 3].number;
                if (t1 < previousTime || t1 > t2 || t2 > endTime)
                    fail(context + "infinite-tangent Bezier control times must be ordered within the endpoints.");
                normalized.push_back(integer(2));
                normalized.push_back(segments[i + 5]);
                normalized.push_back(segments[i + 6]);
                ++repaired;
            } else {
                for (std::size_t j = 0; j < width; ++j) normalized.push_back(segments[i + j]);
            }
            pointCount += type == 1 ? 3 : 1;
            if (++segmentCount > kMaxSegments || pointCount > kMaxPoints) fail("too many segments or points.");
            previousTime = endTime; previousValue = endValue; i += width;
        }
        segments = std::move(normalized);
    }
    // Validate the original metadata before applying the two-point reduction
    // for each converted Bezier. Never hide inconsistent input by recounting it.
    if (segmentCount != expectedSegments) fail("TotalSegmentCount does not match the segments.");
    if (pointCount != expectedPoints) fail("TotalPointCount does not match the original points.");
    member(meta, "TotalPointCount", Value::Number) = integer(pointCount - 2 * repaired);
    const auto* userData = root.find("UserData");
    if (userData && userData->kind != Value::Array) fail("UserData must be an array.");
    const auto eventCount = meta.find("UserDataCount") ? count(meta, "UserDataCount", kMaxSegments) : 0;
    if (eventCount != (userData ? userData->array.size() : 0)) fail("UserDataCount does not match UserData.");
    if (meta.find("TotalUserDataSize")) count(meta, "TotalUserDataSize", kMaxBytes);
    if (auto* events = root.find("UserData")) {
        for (auto& event : events->array) {
            if (member(event, "Time", Value::Number).number < 0) fail("UserData time must be nonnegative.");
            member(event, "Value", Value::String);
        }
    }
    std::string serialized;
    serialized.reserve(input.size() + input.size() / 2);
    serialize(root, serialized);
    return {{serialized.begin(), serialized.end()}, repaired};
}
}
