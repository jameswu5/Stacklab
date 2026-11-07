// Reads TETR.IO's .ttrmx replays: msgpack with msgpackr's records and bundled strings.
// Written from msgpackr's unpacker (MIT, see THIRD_PARTY_NOTICES.md).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

class TtrmxReader {
public:
    explicit TtrmxReader(const std::vector<uint8_t>& bytes) : data_(bytes) {}

    nlohmann::json read() {
        nlohmann::json out = value();
        if (bundled_) pos_ = afterBundle_;
        return out;
    }

private:
    using json = nlohmann::json;

    struct Record {
        std::vector<std::string> keys;
        bool extended = false;  // the id continues in the next byte
    };

    uint8_t byte() {
        if (pos_ >= data_.size()) throw std::runtime_error("ttrmx: data ends early");
        return data_[pos_++];
    }

    uint64_t big(int bytes) {
        uint64_t out = 0;
        for (int i = 0; i < bytes; i++) out = out << 8 | byte();
        return out;
    }

    std::string raw(size_t length) {
        if (length > data_.size() - pos_) throw std::runtime_error("ttrmx: string ends early");
        std::string out(reinterpret_cast<const char*>(data_.data() + pos_), length);
        pos_ += length;
        return out;
    }

    // JavaScript sliced these strings, so lengths count UTF-16 units.
    static std::u16string toUtf16(const std::string& s) {
        std::u16string out;
        for (size_t i = 0; i < s.size();) {
            const uint8_t c = s[i];
            const int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
            uint32_t code = extra == 0 ? c : c & (0x3F >> extra);
            for (int k = 1; k <= extra && i + k < s.size(); k++) code = code << 6 | (s[i + k] & 0x3F);
            i += extra + 1;
            if (code >= 0x10000) {
                out += static_cast<char16_t>(0xD800 + ((code - 0x10000) >> 10));
                out += static_cast<char16_t>(0xDC00 + ((code - 0x10000) & 0x3FF));
            } else {
                out += static_cast<char16_t>(code);
            }
        }
        return out;
    }

    static std::string toUtf8(std::u16string_view s) {
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {
            uint32_t c = s[i];
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
            if (c < 0x80) {
                out += static_cast<char>(c);
            } else if (c < 0x800) {
                out += static_cast<char>(0xC0 | c >> 6);
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out += static_cast<char>(0xE0 | c >> 12);
                out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | c >> 18);
                out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
                out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        return out;
    }

    std::string bundleString() {
        const uint8_t t = byte();
        if (t >= 0xA0 && t < 0xC0) return raw(t - 0xA0);
        if (t == 0xD9) return raw(big(1));
        if (t == 0xDA) return raw(big(2));
        if (t == 0xDB) return raw(big(4));
        throw std::runtime_error("ttrmx: bad string bundle");
    }

    static std::string keyText(const json& key) { return key.is_string() ? key.get<std::string>() : key.dump(); }

    json array(size_t count) {
        json out = json::array();
        for (size_t i = 0; i < count; i++) {
            json item = value();
            out.push_back(item.is_discarded() ? json() : std::move(item));
        }
        return out;
    }

    json map(size_t count) {
        json out = json::object();
        for (size_t i = 0; i < count; i++) {
            const std::string key = keyText(value());
            json item = value();
            if (!item.is_discarded()) out[key] = std::move(item);
        }
        return out;
    }

    json recordBody(const Record& record) {
        json out = json::object();
        for (const std::string& key : record.keys) {
            json item = value();
            if (!item.is_discarded()) out[key] = std::move(item);
        }
        return out;
    }

    static int recordId(int low, int high) { return low < 32 ? -((high << 5) + low) : (high << 5) + low; }

    json recordAt(int low, const Record& record) {
        if (!record.extended) return recordBody(record);
        const uint8_t high = byte();
        if (high == 0) return recordBody(record);
        const auto other = records_.find(recordId(low, high));
        if (other == records_.end()) throw std::runtime_error("ttrmx: unknown record");
        return recordBody(other->second);
    }

    json defineRecord(int low, int high) {
        Record record;
        for (const json& key : value()) record.keys.push_back(keyText(key));
        record.extended = high == 0;
        const int id = high < 0 ? low : recordId(low, high);
        records_[id] = std::move(record);
        // A copy, since reading the body can redefine this id.
        const Record current = records_[id];
        return recordAt(low, current);
    }

    json dateFrom(size_t length) {
        int64_t seconds = 0;
        if (length == 4) {
            seconds = big(4);
        } else if (length == 8) {
            const uint64_t both = big(8);
            seconds = both & 0x3FFFFFFFF;
        } else {
            big(4);
            seconds = static_cast<int64_t>(big(8));
        }
        const std::time_t t = seconds;
        std::tm utc{};
        gmtime_r(&t, &utc);
        char text[32];
        std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%S", &utc);
        return text;
    }

    json extension(size_t length) {
        const uint8_t type = byte();
        if (type == 0xFF) return dateFrom(length);
        if (type != 98 || length < 4) throw std::runtime_error("ttrmx: unknown extension " + std::to_string(type));
        // The strings sit `offset` on, and the body can hold an earlier bundle's strings.
        const size_t start = pos_;
        const size_t after = start + length;
        const size_t offset = big(4);
        pos_ = start + offset;
        strings_[0] = toUtf16(bundleString());
        strings_[1] = toUtf16(bundleString());
        cursor_[0] = cursor_[1] = 0;
        afterBundle_ = pos_;
        bundled_ = true;
        pos_ = after;
        return value();
    }

    json value() {
        const uint8_t t = byte();
        if (t < 0x40) return t;
        if (t < 0x80) {
            const auto record = records_.find(t & 63);
            if (record == records_.end()) return t;
            const Record current = record->second;
            return recordAt(t & 63, current);
        }
        if (t < 0x90) return map(t - 0x80);
        if (t < 0xA0) return array(t - 0x90);
        if (t < 0xC0) return raw(t - 0xA0);
        if (t >= 0xE0) return static_cast<int>(t) - 256;
        switch (t) {
            case 0xC0: return nullptr;
            case 0xC1: {
                if (!bundled_) return json(json::value_t::discarded);
                const int length = value().get<int>();
                const int which = length > 0 ? 1 : 0;
                const size_t count = std::abs(length);
                if (cursor_[which] + count > strings_[which].size()) throw std::runtime_error("ttrmx: bundle ends early");
                const std::string out = toUtf8(std::u16string_view(strings_[which]).substr(cursor_[which], count));
                cursor_[which] += count;
                return out;
            }
            case 0xC2: return false;
            case 0xC3: return true;
            case 0xC4: pos_ += big(1); return nullptr;
            case 0xC5: pos_ += big(2); return nullptr;
            case 0xC6: pos_ += big(4); return nullptr;
            case 0xC7: return extension(big(1));
            case 0xC8: return extension(big(2));
            case 0xC9: return extension(big(4));
            case 0xCA: {
                const uint32_t bits = static_cast<uint32_t>(big(4));
                float f;
                std::memcpy(&f, &bits, sizeof f);
                return static_cast<double>(f);
            }
            case 0xCB: {
                const uint64_t bits = big(8);
                double d;
                std::memcpy(&d, &bits, sizeof d);
                return d;
            }
            case 0xCC: return big(1);
            case 0xCD: return big(2);
            case 0xCE: return big(4);
            case 0xCF: return big(8);
            case 0xD0: return static_cast<int8_t>(big(1));
            case 0xD1: return static_cast<int16_t>(big(2));
            case 0xD2: return static_cast<int32_t>(big(4));
            case 0xD3: return static_cast<int64_t>(big(8));
            case 0xD4: {
                const uint8_t type = byte();
                if (type == 114) {
                    const int low = byte() & 63;
                    return defineRecord(low, -1);
                }
                if (type == 0) {
                    byte();
                    return json(json::value_t::discarded);
                }
                if (type == 1 || type == 2) {
                    byte();
                    json inner = value();
                    json out = json::object();
                    if (type == 2) {
                        out["success"] = false;
                        if (!inner.is_null()) out["error"] = std::move(inner);
                        return out;
                    }
                    if (inner.is_object()) out = std::move(inner);
                    out["success"] = true;
                    return out;
                }
                throw std::runtime_error("ttrmx: unknown extension " + std::to_string(type));
            }
            case 0xD5: {
                if (pos_ < data_.size() && data_[pos_] == 114) {
                    byte();
                    const int low = byte() & 63;
                    const int high = byte();
                    return defineRecord(low, high);
                }
                return extension(2);
            }
            case 0xD6: return extension(4);
            case 0xD7: return extension(8);
            case 0xD8: return extension(16);
            case 0xD9: return raw(big(1));
            case 0xDA: return raw(big(2));
            case 0xDB: return raw(big(4));
            case 0xDC: return array(big(2));
            case 0xDD: return array(big(4));
            case 0xDE: return map(big(2));
            case 0xDF: return map(big(4));
        }
        throw std::runtime_error("ttrmx: unknown token");
    }

    const std::vector<uint8_t>& data_;
    size_t pos_ = 0;
    std::map<int, Record> records_;
    std::u16string strings_[2];
    size_t cursor_[2] = {0, 0};
    size_t afterBundle_ = 0;
    bool bundled_ = false;
};
