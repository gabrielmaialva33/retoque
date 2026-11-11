#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

struct ImGuiIO;

// A menu text pack in Japanese, Korean or Chinese needs glyphs the menu's fonts lack (Hack and ImGui's
// ProggyClean cover Latin, Greek and Cyrillic). The portable part below decides, from the active
// OptiScaler.lang alone, which script it is and which codepoints it uses; host tests run it. With no
// pack, or a pack without those characters (pt-BR, de, es, fr, pl, ru), the plan is empty and the menu's
// atlas is left exactly as upstream builds it. See retoque/translations/README.md.
namespace MenuFonts
{
enum class Script
{
    None,
    Japanese,
    Korean,
    Chinese,
};

// Faces Windows ships under %WINDIR%\Fonts, and Proton links into the prefix's C:\windows\Fonts
// (malgun.ttf, msgothic.ttc, msyh.ttf, simsun.ttc), in order of preference. Index 0 of each .ttc is
// the regular face of the family named here.
struct FontFile
{
    const wchar_t* name;
    int faceIndex;
};

inline const std::vector<FontFile>& Candidates(Script script)
{
    static const std::vector<FontFile> japanese {
        { L"YuGothM.ttc", 0 }, { L"meiryo.ttc", 0 }, { L"msgothic.ttc", 0 }, { L"YuGothR.ttc", 0 }
    };
    static const std::vector<FontFile> korean { { L"malgun.ttf", 0 }, { L"gulim.ttc", 0 } };
    static const std::vector<FontFile> chinese {
        { L"msyh.ttc", 0 }, { L"msyh.ttf", 0 }, { L"simsun.ttc", 0 }, { L"Deng.ttf", 0 }, { L"simhei.ttf", 0 }
    };
    static const std::vector<FontFile> none;
    switch (script)
    {
    case Script::Japanese:
        return japanese;
    case Script::Korean:
        return korean;
    case Script::Chinese:
        return chinese;
    default:
        return none;
    }
}

inline const char* ScriptName(Script script)
{
    switch (script)
    {
    case Script::Japanese:
        return "Japanese";
    case Script::Korean:
        return "Korean";
    case Script::Chinese:
        return "Chinese";
    default:
        return "none";
    }
}

inline bool IsHangul(uint32_t c)
{
    return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) || (c >= 0xA960 && c <= 0xA97F) ||
           (c >= 0xAC00 && c <= 0xD7FF);
}

inline bool IsKana(uint32_t c) { return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF); }

// Codepoints a merged CJK face supplies: Hangul, kana, CJK ideographs, CJK symbols and punctuation,
// enclosed and compatibility forms, and the fullwidth/halfwidth forms CJK text uses.
inline bool NeedsCjkFont(uint32_t c)
{
    return IsHangul(c) || (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF);
}

class Plan
{
    std::vector<uint32_t> points_;
    bool hangul_ = false;
    bool kana_ = false;

  public:
    // Text is UTF-8 the dictionary already validated; a malformed sequence is skipped, never trusted.
    void Add(std::string_view text)
    {
        for (size_t i = 0; i < text.size();)
        {
            const auto lead = static_cast<unsigned char>(text[i]);
            const size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (i + length > text.size())
                return;
            uint32_t c = length == 1 ? lead : lead & (0x7F >> length);
            for (size_t j = 1; j < length; ++j)
                c = (c << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3F);
            i += length;
            if (!NeedsCjkFont(c))
                continue;
            hangul_ |= IsHangul(c);
            kana_ |= IsKana(c);
            points_.push_back(c);
        }
    }

    // Hangul means Korean, kana Japanese, ideographs alone Chinese: the face's regional glyph forms
    // follow the language, and a Korean pack may quote a Han character.
    Script Needed() const
    {
        if (points_.empty())
            return Script::None;
        return hangul_ ? Script::Korean : kana_ ? Script::Japanese : Script::Chinese;
    }

    // Inclusive [first, last] pairs over exactly the codepoints the pack uses, sorted and merged, ending
    // with 0 as ImFontConfig::GlyphRanges wants. Empty when nothing is needed. ImGui here stores 16-bit
    // codepoints, so anything above U+FFFF is left out (and could not be drawn anyway).
    std::vector<uint32_t> Ranges() const
    {
        std::vector<uint32_t> sorted;
        for (const auto c : points_)
            if (c <= 0xFFFF)
                sorted.push_back(c);
        if (sorted.empty())
            return {};
        std::sort(sorted.begin(), sorted.end());
        std::vector<uint32_t> ranges;
        for (const auto c : sorted)
        {
            if (!ranges.empty() && c <= ranges.back() + 1)
                ranges.back() = c;
            else
            {
                ranges.push_back(c);
                ranges.push_back(c);
            }
        }
        ranges.push_back(0);
        return ranges;
    }
};

// The first candidate for the script that exists, or nullptr: then the atlas stays as it is and the
// characters the menu's font lacks show as ImGui's fallback glyph.
template <typename Exists> const FontFile* Choose(Script script, Exists&& exists)
{
    for (const auto& font : Candidates(script))
        if (exists(font.name))
            return &font;
    return nullptr;
}

// Windows side (MenuFonts.cpp): merges the chosen system face into the menu's default font when the
// active OptiScaler.lang needs it. Call after the menu has added its own font; repeated calls do nothing.
void AddTranslationFallback(ImGuiIO& io);
} // namespace MenuFonts
