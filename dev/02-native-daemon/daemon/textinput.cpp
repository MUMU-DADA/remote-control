// textinput.cpp — UTF-8 解码与 US 键盘布局映射

#include "textinput.h"

#include <linux/input.h>

namespace remote_control {
namespace textinput {
namespace {

// 可打印 ASCII 的偏移量：'1'..'9','0' 是连续的键码。
constexpr uint32_t kCharA = 'a';
constexpr uint32_t kCharZ = 'z';

// 字母键码**不是按字母表连续的**，不能写成 KEY_A + ('z'-'a')。
//
// 键盘的物理排列是三行错位的：
//   上排 Q W E R T Y U I O P  = 16..25
//   中排 A S D F G H J K L    = 30..38
//   下排 Z X C V B N M        = 44..50   ← Z 在 X 前面，M 在最后
// 所以 'z' 是 44 而不是 30+25=55，'m' 是 50 而不是 42。
//
// ⚠️ 写成算术式的版本不会崩、也不会报错，只会**打出一个错字**
//    （按 z 出来的是别的键）。test_textinput 就是为这个写的 ——
//    第一版正是栽在这里，74 项里挂了 3 项。
constexpr uint32_t kLetterKeycode[26] = {
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
    KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
    KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
};

}  // namespace

// ── UTF-8 解码 ──────────────────────────────────────────────────────────────
//
// 手写而不是用第三方：只需要"码点 + 起始偏移 + 非法计数"这三样，
// 引一个库不划算。严格拒绝超长编码与代理区（U+D800–DFFF）——
// 放行它们会让同一段文本有两种字节表示，后面按字节比对时会出错。
Decoded Decode(const std::string& s) {
    Decoded out;
    const size_t n = s.size();
    size_t i = 0;

    while (i < n) {
        const size_t start = i;
        const uint8_t b0 = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        size_t need = 0;

        if (b0 < 0x80) {
            cp = b0;
            need = 0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F;
            need = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F;
            need = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07;
            need = 3;
        } else {
            ++out.invalidBytes;
            ++i;
            continue;
        }

        // 后续字节不够（i+1 .. i+need 都必须存在 → 需要 i+need <= n-1）
        if (need > 0 && i + need >= n) {
            out.invalidBytes += n - i;
            break;
        }

        bool bad = false;
        for (size_t k = 1; k <= need; ++k) {
            const uint8_t bk = static_cast<uint8_t>(s[i + k]);
            if ((bk & 0xC0) != 0x80) {
                bad = true;
                break;
            }
            cp = (cp << 6) | (bk & 0x3F);
        }
        if (bad) {
            ++out.invalidBytes;
            ++i;
            continue;
        }

        // 超长编码 / 代理区 / 超出范围一律判非法
        static const uint32_t kMinForLen[4] = {0, 0x80, 0x800, 0x10000};
        if (cp < kMinForLen[need] || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.invalidBytes += need + 1;
            i += need + 1;
            continue;
        }

        out.codepoints.push_back(cp);
        out.offsets.push_back(start);
        i += need + 1;
    }
    return out;
}

std::string Encode(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

// ── US 布局映射 ─────────────────────────────────────────────────────────────
//
// 只覆盖可打印 ASCII 加 \n \t。这不是偷懒：Linux 键码空间里**根本没有**
// 表达"中"这个字的办法（键码是物理按键编号，字符是布局解释出来的），
// 所以任何"用按键打中文"的方案都注定失败 —— Android 自己的
// `input text` 就是撞在这上面，对中文直接抛 NullPointerException。
// 中文走 paste 模式。
bool MapChar(uint32_t c, uint32_t* keycode, bool* shift) {
    if (keycode == nullptr || shift == nullptr) return false;
    *shift = false;

    // 字母。查表而不是算偏移 —— 理由见 kLetterKeycode 的注释。
    if (c >= 'a' && c <= kCharZ) { *keycode = kLetterKeycode[c - kCharA]; return true; }
    if (c >= 'A' && c <= 'Z')    { *keycode = kLetterKeycode[c - 'A']; *shift = true; return true; }

    // 数字：'1'..'9' 然后是 '0'（键码顺序如此，第 10 个键才是 0）
    if (c >= '1' && c <= '9') { *keycode = KEY_1 + (c - '1'); return true; }
    if (c == '0')             { *keycode = KEY_0; return true; }

    switch (c) {
        case ' ':  *keycode = KEY_SPACE; return true;
        case '\n': *keycode = KEY_ENTER; return true;
        case '\t': *keycode = KEY_TAB;   return true;

        // 上档符号：与数字同键
        case '!': *keycode = KEY_1; *shift = true; return true;
        case '@': *keycode = KEY_2; *shift = true; return true;
        case '#': *keycode = KEY_3; *shift = true; return true;
        case '$': *keycode = KEY_4; *shift = true; return true;
        case '%': *keycode = KEY_5; *shift = true; return true;
        case '^': *keycode = KEY_6; *shift = true; return true;
        case '&': *keycode = KEY_7; *shift = true; return true;
        case '*': *keycode = KEY_8; *shift = true; return true;
        case '(': *keycode = KEY_9; *shift = true; return true;
        case ')': *keycode = KEY_0; *shift = true; return true;

        // 标点
        case '-':  *keycode = KEY_MINUS;      return true;
        case '_':  *keycode = KEY_MINUS;      *shift = true; return true;
        case '=':  *keycode = KEY_EQUAL;      return true;
        case '+':  *keycode = KEY_EQUAL;      *shift = true; return true;
        case '[':  *keycode = KEY_LEFTBRACE;  return true;
        case '{':  *keycode = KEY_LEFTBRACE;  *shift = true; return true;
        case ']':  *keycode = KEY_RIGHTBRACE; return true;
        case '}':  *keycode = KEY_RIGHTBRACE; *shift = true; return true;
        case '\\': *keycode = KEY_BACKSLASH;  return true;
        case '|':  *keycode = KEY_BACKSLASH;  *shift = true; return true;
        case ';':  *keycode = KEY_SEMICOLON;  return true;
        case ':':  *keycode = KEY_SEMICOLON;  *shift = true; return true;
        case '\'': *keycode = KEY_APOSTROPHE; return true;
        case '"':  *keycode = KEY_APOSTROPHE; *shift = true; return true;
        case '`':  *keycode = KEY_GRAVE;      return true;
        case '~':  *keycode = KEY_GRAVE;      *shift = true; return true;
        case ',':  *keycode = KEY_COMMA;      return true;
        case '<':  *keycode = KEY_COMMA;      *shift = true; return true;
        case '.':  *keycode = KEY_DOT;        return true;
        case '>':  *keycode = KEY_DOT;        *shift = true; return true;
        case '/':  *keycode = KEY_SLASH;      return true;
        case '?':  *keycode = KEY_SLASH;      *shift = true; return true;
        default: return false;
    }
}

bool FirstUntypable(const std::string& utf8, uint32_t* codepoint,
                    size_t* charIndex) {
    const Decoded d = Decode(utf8);
    for (size_t i = 0; i < d.codepoints.size(); ++i) {
        uint32_t code = 0;
        bool shift = false;
        if (!MapChar(d.codepoints[i], &code, &shift)) {
            if (codepoint) *codepoint = d.codepoints[i];
            if (charIndex) *charIndex = i;
            return true;
        }
    }
    return false;
}

}  // namespace textinput
}  // namespace remote_control
