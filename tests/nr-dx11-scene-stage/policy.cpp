#include "pch.h"
#include "DlssNr_Dx11ScenePolicy.h"

int main()
{
    using namespace DlssNr::Dx11Scene;
    assert(Eligible(true, true, true, 1, 0));
    assert(!Eligible(false, true, true, 1, 0));
    assert(!Eligible(true, false, true, 1, 0));
    assert(!Eligible(true, true, false, 1, 0));
    assert(!Eligible(true, true, true, 2, 0));
    assert(!Eligible(true, true, true, 0, 0));
    assert(!Eligible(true, true, true, 1, 1));
    assert(!Complete(UINT64_MAX, 1));
    assert(!Complete(4, 5));
    assert(Complete(5, 5));
    assert(QueueRefusal(0, 2, 3));
    assert(QueueRefusal(1, 0, 3));
    assert(QueueRefusal(1, 2, 0));
    assert(QueueRefusal(1, 1, 3));
    assert(QueueRefusal(1, 2, 1));
    assert(!QueueRefusal(1, 2, 3));
    assert(MaxBytes == 512ull * 1024 * 1024);
}
