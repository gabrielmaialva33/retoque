// [DlssNr] ModelFile's rule (retoque/dlssnr/DlssNr_ModelFile.h): auto, a named file found beside OptiScaler or the
// game, an absolute path, and a named file that is not there, which is refused and never falls back. The
// filesystem is a set of paths; nothing is loaded.
#include "retoque/dlssnr/DlssNr_ModelFile.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <string>

using namespace DlssNr::ModelFile;
namespace fs = std::filesystem;

namespace
{
const fs::path kOpti = "/games/Title/bin/x64";
const fs::path kExe = "/games/Title/bin";

struct Files
{
    std::set<std::string> files;
    bool operator()(const fs::path& p) const { return files.count(p.lexically_normal().generic_string()) != 0; }
};

void AutoIsTheDefault()
{
    for (const char* value : { "auto", "AUTO", "  Auto ", "", "   " })
        assert(IsAuto(value));
    for (const char* value : { "nvngx_dlssnr.dll", "autox", "auto.dll", "DlssNrModels/a/nvngx_dlssnr.dll" })
        assert(!IsAuto(value));
}

void NamedBesideOptiScalerFirst()
{
    Files fs { { "/games/Title/bin/x64/nvngx_dlssnr_310.8.dll", "/games/Title/bin/nvngx_dlssnr_310.8.dll" } };
    const Named n = Resolve("nvngx_dlssnr_310.8.dll", kOpti, kExe, fs);
    assert(n.path.generic_string() == "/games/Title/bin/x64/nvngx_dlssnr_310.8.dll");
    assert(n.label == "nvngx_dlssnr_310.8.dll" && n.refusal.empty());
}

void NamedBesideTheGame()
{
    Files fs { { "/games/Title/bin/DlssNrModels/310.8/nvngx_dlssnr.dll" } };
    const Named n = Resolve("DlssNrModels\\310.8\\nvngx_dlssnr.dll", kOpti, kExe, fs);
    assert(n.path.generic_string() == "/games/Title/bin/DlssNrModels/310.8/nvngx_dlssnr.dll");
    assert(n.label == "DlssNrModels/310.8/nvngx_dlssnr.dll");
}

void MissingIsRefusedNeverTheDefault()
{
    // The default model is there; a named one that is not must not fall back to it.
    Files fs { { "/games/Title/bin/x64/nvngx_dlssnr.dll" } };
    const Named n = Resolve("nvngx_dlssnr_310.8.dll", kOpti, kExe, fs);
    assert(n.path.empty());
    assert(n.refusal.find("nvngx_dlssnr_310.8.dll") != std::string::npos);
    assert(n.refusal.find("does not fall back") != std::string::npos);
    // A relative value that climbs out of the folders does not put the folders it names in the reason.
    const Named up = Resolve("../../elsewhere/someone/models/nvngx_dlssnr.dll", kOpti, kExe, fs);
    assert(up.path.empty() && up.refusal.find("someone") == std::string::npos);
}

void AbsoluteAsGiven()
{
    // libstdc++ folds the UNC prefix "//" to "/" when it spells a path; MSVC keeps it. The share is listed both ways.
    Files fs { { "D:/someone/models/310.8/nvngx_dlssnr.dll", "//server/share/someone/nvngx_dlssnr_unc.dll",
                 "/server/share/someone/nvngx_dlssnr_unc.dll" } };
    const Named n = Resolve("D:\\someone\\models\\310.8\\nvngx_dlssnr.dll", kOpti, kExe, fs);
    assert(n.path.generic_string() == "D:/someone/models/310.8/nvngx_dlssnr.dll");
    // A folder of the user's own is not written out: the file name only.
    assert(n.label == "nvngx_dlssnr.dll");
    const Named unc = Resolve("\\\\server\\share\\someone\\nvngx_dlssnr_unc.dll", kOpti, kExe, fs);
    assert(!unc.path.empty() && unc.label == "nvngx_dlssnr_unc.dll");
    const Named missing = Resolve("D:/someone/models/gone.dll", kOpti, kExe, fs);
    assert(missing.path.empty() && missing.refusal.find("someone") == std::string::npos);
    assert(missing.refusal.find("gone.dll") != std::string::npos);
}

void PartialRootsRefused()
{
    // "C:x" is the drive's current folder and "\\x" the current drive's root: both depend on the working directory,
    // so they are refused even where such a file would be found.
    Files fs { { "C:nvngx_dlssnr_cwd.dll", "/nvngx_dlssnr_root.dll", "/games/Title/bin/x64/C:nvngx_dlssnr_cwd.dll" } };
    for (const char* value : { "C:nvngx_dlssnr_cwd.dll", "\\nvngx_dlssnr_root.dll", "/nvngx_dlssnr_root.dll" })
    {
        const Named n = Resolve(value, kOpti, kExe, fs);
        assert(n.path.empty());
        assert(n.refusal.find("not a full path") != std::string::npos);
    }
    assert(RootingOf("C:\\m.dll") == Rooting::Full && RootingOf("c:/m.dll") == Rooting::Full);
    assert(RootingOf("\\\\s\\m.dll") == Rooting::Full && RootingOf("//s/m.dll") == Rooting::Full);
    assert(RootingOf("C:m.dll") == Rooting::Partial && RootingOf("C:") == Rooting::Partial);
    assert(RootingOf("\\m.dll") == Rooting::Partial && RootingOf("m.dll") == Rooting::Relative);
}

void Utf8Paths()
{
    // The INI is UTF-8; the path is made from those bytes, and the label keeps them.
    const std::string name = "Modelos/vers\xC3\xA3o/nvngx_dlssnr.dll"; // versao with a tilde, as UTF-8
    Files fs { { "/games/Title/bin/" + name } };
    const Named n = Resolve(name, kOpti, kExe, fs);
    assert(!n.path.empty() && n.label == name);
    assert(Utf8(FromUtf8(name)) == name);
}

void WhitespaceTrimmed()
{
    Files fs { { "/games/Title/bin/x64/nvngx_dlssnr_b.dll" } };
    assert(!Resolve("  nvngx_dlssnr_b.dll\t", kOpti, kExe, fs).path.empty());
}

void ADirectoryIsNotAModel()
{
    // isRegularFile says no for a folder named like a model, so it is not taken.
    Files fs {};
    assert(Resolve("nvngx_dlssnr.dll", kOpti, kExe, fs).path.empty());
}

void EmptyFoldersAreSkipped()
{
    Files fs { { "/games/Title/bin/nvngx_dlssnr_c.dll" } };
    const Named n = Resolve("nvngx_dlssnr_c.dll", fs::path(), kExe, fs);
    assert(n.path.generic_string() == "/games/Title/bin/nvngx_dlssnr_c.dll");
    // No relative path is tried against the working directory.
    Files cwd { { "nvngx_dlssnr_c.dll" } };
    assert(Resolve("nvngx_dlssnr_c.dll", fs::path(), fs::path(), cwd).path.empty());
}

void LabelStaysInside()
{
    assert(Label("/games/Title/bin/x64/a/b.dll", kOpti, kExe) == "a/b.dll");
    assert(Label("/games/Title/bin/c.dll", kOpti, kExe) == "c.dll");
    assert(Label("/elsewhere/d.dll", kOpti, kExe) == "d.dll");
    assert(!Inside("/games/Title/bin/x64/../../other/e.dll", kOpti));
    assert(!Inside("/games/Title/bin/x64", kOpti)); // the folder itself is not a file inside it
}
} // namespace

int main()
{
    AutoIsTheDefault();
    NamedBesideOptiScalerFirst();
    NamedBesideTheGame();
    MissingIsRefusedNeverTheDefault();
    AbsoluteAsGiven();
    PartialRootsRefused();
    Utf8Paths();
    WhitespaceTrimmed();
    ADirectoryIsNotAModel();
    EmptyFoldersAreSkipped();
    LabelStaysInside();
    std::puts("nr-model-file: 11 cases passed");
    return 0;
}
