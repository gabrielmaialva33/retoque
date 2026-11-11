// The menu font decision (retoque/misc/MenuFonts.h) over every shipped pack: which script a pack needs,
// that the merged face's ranges plus the menu's Hack font cover every character the pack draws, and that a
// missing system font leaves the choice empty (the atlas is then untouched).
#include "misc/Localization.h"
#include "misc/MenuFonts.h"

#include <cassert>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

using MenuFonts::Plan;
using MenuFonts::Script;

static std::set<uint32_t> ReadCodepoints(const char* path)
{
    std::ifstream file(path);
    std::set<uint32_t> points;
    for (uint32_t c; file >> c;)
        points.insert(c);
    return points;
}

static std::vector<uint32_t> Decode(std::string_view text)
{
    std::vector<uint32_t> points;
    for (size_t i = 0; i < text.size();)
    {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        uint32_t c = length == 1 ? lead : lead & (0x7F >> length);
        for (size_t j = 1; j < length; ++j)
            c = (c << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3F);
        points.push_back(c);
        i += length;
    }
    return points;
}

static bool InRanges(const std::vector<uint32_t>& ranges, uint32_t c)
{
    for (size_t i = 0; i + 1 < ranges.size(); i += 2)
        if (c >= ranges[i] && c <= ranges[i + 1])
            return true;
    return false;
}

static Script Expected(const std::string& locale)
{
    if (locale == "ja")
        return Script::Japanese;
    if (locale == "ko")
        return Script::Korean;
    if (locale == "zh-CN")
        return Script::Chinese;
    return Script::None;
}

int main(int argc, char** argv)
{
    assert(argc >= 3);
    const auto hack = ReadCodepoints(argv[1]);
    assert(hack.size() > 1000 && hack.count(0x0416) && !hack.count(0x4E2D)); // Cyrillic yes, Han no.

    // No pack: nothing needed, no ranges, no font looked up.
    Plan empty;
    assert(empty.Needed() == Script::None && empty.Ranges().empty());
    Plan latin;
    latin.Add("Configurações ąęłńśźż Привет «» – … ß œ");
    assert(latin.Needed() == Script::None && latin.Ranges().empty());

    // Script choice: Hangul wins (Korean may quote Hanja), then kana, then ideographs alone.
    Plan chinese, japanese, korean;
    chinese.Add("帧生成：开");
    japanese.Add("フレーム生成");
    korean.Add("프레임 生成");
    assert(chinese.Needed() == Script::Chinese && japanese.Needed() == Script::Japanese &&
           korean.Needed() == Script::Korean);
    // Sorted, merged, zero-terminated ranges over exactly the used codepoints.
    Plan ranges;
    ranges.Add("\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAC\xE4\xB8\xAE"); // U+4E2D twice, U+4E2C, U+4E2E
    assert((ranges.Ranges() == std::vector<uint32_t> { 0x4E2C, 0x4E2E, 0 }));
    Plan astral;
    astral.Add("\xF0\xA0\x80\x80"); // U+20000: needs a CJK face but a 16-bit ImWchar cannot hold it
    assert(astral.Needed() == Script::None || astral.Ranges().empty());

    // Every script has candidates, Proton's prefix fonts among them; none present means no font.
    for (const auto script : { Script::Japanese, Script::Korean, Script::Chinese })
    {
        assert(!MenuFonts::Candidates(script).empty());
        assert(MenuFonts::Choose(script, [](const wchar_t*) { return false; }) == nullptr);
    }
    assert(MenuFonts::Candidates(Script::None).empty());
    assert(MenuFonts::Choose(Script::None, [](const wchar_t*) { return true; }) == nullptr);
    const wchar_t* proton[] = { L"malgun.ttf", L"msgothic.ttc", L"msyh.ttf", L"simsun.ttc" };
    auto inProton = [&](const wchar_t* name)
    {
        for (const auto* file : proton)
            if (std::wcscmp(file, name) == 0)
                return true;
        return false;
    };
    assert(std::wcscmp(MenuFonts::Choose(Script::Japanese, inProton)->name, L"msgothic.ttc") == 0);
    assert(std::wcscmp(MenuFonts::Choose(Script::Korean, inProton)->name, L"malgun.ttf") == 0);
    assert(std::wcscmp(MenuFonts::Choose(Script::Chinese, inProton)->name, L"msyh.ttf") == 0);
    assert(std::wcscmp(MenuFonts::Choose(Script::Chinese, [](const wchar_t*) { return true; })->name, L"msyh.ttc") ==
           0);

    for (int i = 2; i < argc; ++i)
    {
        const std::string argument(argv[i]);
        const auto equals = argument.find('=');
        assert(equals != std::string::npos);
        const std::string locale = argument.substr(0, equals);
        std::ifstream file(argument.substr(equals + 1), std::ios::binary);
        Localization::Dictionary dictionary;
        assert(file && dictionary.Load(file));
        Plan plan;
        dictionary.ForEachTranslation([&](std::string_view text) { plan.Add(text); });
        if (plan.Needed() != Expected(locale))
        {
            std::cerr << locale << ": expected " << MenuFonts::ScriptName(Expected(locale)) << ", got "
                      << MenuFonts::ScriptName(plan.Needed()) << "\n";
            return 1;
        }
        const auto planRanges = plan.Ranges();
        assert(planRanges.empty() == (Expected(locale) == Script::None));
        size_t missing = 0, merged = 0;
        dictionary.ForEachTranslation(
            [&](std::string_view text)
            {
                for (const auto c : Decode(text))
                {
                    if (c == '\n' || hack.count(c))
                        continue;
                    if (InRanges(planRanges, c))
                    {
                        ++merged;
                        continue;
                    }
                    if (missing++ < 10)
                        std::cerr << locale << ": U+" << std::hex << c << std::dec
                                  << " is neither in the menu font nor in the merged ranges\n";
                }
            });
        if (missing)
            return 1;
        std::cout << "PASS: " << locale << " needs " << MenuFonts::ScriptName(plan.Needed()) << ", "
                  << (planRanges.empty() ? 0 : planRanges.size() / 2) << " merged ranges, " << merged
                  << " characters from the merged face, every other character in Hack\n";
    }
}
