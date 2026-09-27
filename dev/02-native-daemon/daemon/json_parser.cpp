// json_parser.cpp — 极简 JSON 解析器

#include "json_parser.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace autod {
namespace json {
namespace {

class Parser {
  public:
    Parser(const std::string& s) : s_(s) {}

    bool Run(Value* out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        if (pos_ != s_.size()) return Fail("末尾有多余内容");
        return true;
    }

    const std::string& error() const { return error_; }

  private:
    bool Fail(const char* why) {
        if (error_.empty()) {
            error_ = std::string(why) + "（位置 " + std::to_string(pos_) + "）";
        }
        return false;
    }

    void SkipWs() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos_; continue; }
            break;
        }
    }

    bool Literal(const char* lit) {
        const size_t n = strlen(lit);
        if (s_.compare(pos_, n, lit) != 0) return false;
        pos_ += n;
        return true;
    }

    bool ParseValue(Value* out) {
        if (pos_ >= s_.size()) return Fail("期待一个值，但已到末尾");
        const char c = s_[pos_];
        switch (c) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                std::string str;
                if (!ParseString(&str)) return false;
                out->SetString(std::move(str));
                return true;
            }
            case 't':
                if (Literal("true"))  { out->SetBool(true);  return true; }
                return Fail("期待 true");
            case 'f':
                if (Literal("false")) { out->SetBool(false); return true; }
                return Fail("期待 false");
            case 'n':
                if (Literal("null"))  { out->SetNull();      return true; }
                return Fail("期待 null");
            default:
                return ParseNumber(out);
        }
    }

    bool ParseObject(Value* out) {
        ++pos_;                       // '{'
        out->SetObject();
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }

        for (;;) {
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("对象的键必须是字符串");
            std::string key;
            if (!ParseString(&key)) return false;

            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("键之后期待 ':'");
            ++pos_;

            SkipWs();
            Value v;
            if (!ParseValue(&v)) return false;
            out->Put(key, std::move(v));

            SkipWs();
            if (pos_ >= s_.size()) return Fail("对象没有闭合");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return Fail("对象里期待 ',' 或 '}'");
        }
    }

    bool ParseArray(Value* out) {
        ++pos_;                       // '['
        out->SetArray();
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }

        for (;;) {
            SkipWs();
            Value v;
            if (!ParseValue(&v)) return false;
            out->Push(std::move(v));

            SkipWs();
            if (pos_ >= s_.size()) return Fail("数组没有闭合");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return Fail("数组里期待 ',' 或 ']'");
        }
    }

    // 读取一个 \uXXXX，返回码点；失败返回 -1
    int ParseHex4() {
        if (pos_ + 4 > s_.size()) return -1;
        int v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_ + i];
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return -1;
            v = v * 16 + d;
        }
        pos_ += 4;
        return v;
    }

    static void AppendUtf8(std::string* out, uint32_t cp) {
        if (cp < 0x80) {
            *out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            *out += static_cast<char>(0xC0 | (cp >> 6));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *out += static_cast<char>(0xE0 | (cp >> 12));
            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            *out += static_cast<char>(0xF0 | (cp >> 18));
            *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            *out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool ParseString(std::string* out) {
        ++pos_;                       // 开引号
        out->clear();
        while (pos_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') { ++pos_; return true; }

            if (c != '\\') {
                // 裸控制字符在 JSON 里非法。放过它会让产出的字符串
                // 在别处出问题，不如当场报错。
                if (c < 0x20) return Fail("字符串里有未转义的控制字符");
                *out += static_cast<char>(c);
                ++pos_;
                continue;
            }

            ++pos_;                   // 反斜杠
            if (pos_ >= s_.size()) return Fail("转义符后没有内容");
            const char e = s_[pos_++];
            switch (e) {
                case '"':  *out += '"';  break;
                case '\\': *out += '\\'; break;
                case '/':  *out += '/';  break;
                case 'b':  *out += '\b'; break;
                case 'f':  *out += '\f'; break;
                case 'n':  *out += '\n'; break;
                case 'r':  *out += '\r'; break;
                case 't':  *out += '\t'; break;
                case 'u': {
                    int cp = ParseHex4();
                    if (cp < 0) return Fail("\\u 后面不是 4 位十六进制");
                    // 代理对：高位 + 低位合成一个码点
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        pos_ + 1 < s_.size() && s_[pos_] == '\\' &&
                        s_[pos_ + 1] == 'u') {
                        const size_t save = pos_;
                        pos_ += 2;
                        const int lo = ParseHex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            pos_ = save;   // 不是合法低位，当作独立的 \u 处理
                        }
                    }
                    AppendUtf8(out, static_cast<uint32_t>(cp));
                    break;
                }
                default:
                    return Fail("不认识的转义符");
            }
        }
        return Fail("字符串没有闭合");
    }

    bool ParseNumber(Value* out) {
        const size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        bool any = false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; any = true; }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; any = true; }
        }
        if (any && pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (!any) return Fail("不是一个合法的值");

        const std::string numText = s_.substr(start, pos_ - start);
        out->SetNumber(strtod(numText.c_str(), nullptr));
        return true;
    }

    const std::string& s_;
    size_t      pos_ = 0;
    std::string error_;
};

}  // namespace

bool Parse(const std::string& text, Value* out, std::string* error) {
    if (out == nullptr) return false;
    *out = Value{};
    Parser p(text);
    if (p.Run(out)) return true;
    if (error != nullptr) *error = p.error();
    return false;
}

}  // namespace json
}  // namespace autod
