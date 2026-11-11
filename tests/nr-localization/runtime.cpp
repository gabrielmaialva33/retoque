#include "misc/Localization.h"
#include "misc/MenuFonts.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    assert(argc == 2);
    // The menu font decision over the dictionary this process actually loaded: no pack, an inactive or invalid
    // one, and pt-BR leave the atlas untouched; an active Japanese pack asks for a Japanese face.
    MenuFonts::Plan plan;
    Localization::Active().ForEachTranslation([&](std::string_view text) { plan.Add(text); });
    if (std::string(argv[1]) == "active-ja")
    {
        assert(Localization::Active().Size() > 100);
        assert(plan.Needed() == MenuFonts::Script::Japanese && !plan.Ranges().empty());
        assert(std::string(Localization::Tr("Save Settings")) != "Save Settings");
        std::cout << "PASS: real runtime loader active-ja; Japanese menu font requested, "
                  << plan.Ranges().size() / 2 << " ranges\n";
        return 0;
    }
    assert(plan.Needed() == MenuFonts::Script::None && plan.Ranges().empty());
    const bool active = std::string(argv[1]) == "active";
    if (!active)
        assert(Localization::Active().Size() == 0);
    const char* original = "Save Settings";
    const char* translated = Localization::Tr(original);
    if (!active) assert(translated == original);
    else assert(std::string(translated) == "Salvar configurações");
    assert(std::string(Localization::Label("Reset##first")) ==
           (active ? "Redefinir###Reset##first" : "Reset##first"));
    assert(std::string(Localization::Label("Reset###first")) ==
           (active ? "Redefinir###first" : "Reset###first"));
    assert(std::string(Localization::Tr("Reset")) == (active ? "Redefinir" : "Reset"));
    // Loading is once per process. Editing/deleting the file cannot invalidate returned pointers.
    std::ofstream replacement("OptiScaler.lang");
    replacement << "Save Settings\tAnother translation\n";
    replacement.close();
    assert(Localization::Tr(original) == translated);
    std::cout << "PASS: real runtime loader " << argv[1] << "; exact display lookup, immutable pointers and IDs\n";
}
