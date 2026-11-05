#include <dlssnr/DlssNr_VkPreScope.h>
#include <cassert>
#include <thread>
int main()
{
    using DlssNr::VkPre::Placement;
    Placement pre;
    assert(pre.TryPre() && pre.TryPre());
    assert(pre.PostNeedsLoan());
    Placement post;
    assert(!post.PostNeedsLoan() && !post.PostNeedsLoan());
    assert(!post.TryPre());
    for (int i = 0; i < 100; ++i)
    {
        Placement race;
        bool admitted = false, loan = false;
        std::thread a([&] { admitted = race.TryPre(); });
        std::thread b([&] { loan = race.PostNeedsLoan(); });
        a.join();
        b.join();
        assert(admitted == loan); // pre wins => post borrows; post wins => pre refuses, never both unguarded
    }
}
