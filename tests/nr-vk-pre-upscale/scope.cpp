#include <dlssnr/DlssNr_VkPreScope.h>
#include <cassert>
#include <stdexcept>
#include <string_view>
struct Params
{
    void* colour;
    int other = 7;
    void Set(const char* key, void* value)
    {
        assert(std::string_view(key) == "Color");
        colour = value;
    }
};
int main()
{
    int original = 1, scratch = 2;
    for (bool failure : { false, true })
    {
        Params p { &original };
        int nr = 0, sr = 0, binding = 11;
        try
        {
            DlssNr::VkPre::ColourLoan<Params> loan;
            ++nr;
            binding = 22;
            loan.Bind(&p, &original, &scratch);
            ++sr;
            assert(nr == 1 && p.colour == &scratch);
            binding = 33;
            p.other = 8; // SR establishes its own state under the NGX contract.
            if (failure)
                throw std::runtime_error("provider");
            loan.Restore(); // before UpscaleEnd or any subsequent return
            assert(p.colour == &original);
        }
        catch (const std::runtime_error&)
        {
        }
        assert(p.colour == &original && p.other == 8 && nr == 1 && sr == 1 && binding == 33);
        binding = 44; // the game's own post-evaluate rebind, followed by its sentinel dispatch
        assert(binding == 44 && nr == 1);
    }
    Params off { &original };
    {
        DlssNr::VkPre::ColourLoan<Params> unused;
    }
    assert(off.colour == &original);
}
