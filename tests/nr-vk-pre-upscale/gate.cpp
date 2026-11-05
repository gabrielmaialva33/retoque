#include <dlssnr/DlssNr_DriverInitGate.h>
#include <cassert>
#include <thread>
using namespace DlssNr::DriverInitGate;
int main()
{
    Gate gate;
    assert(gate.Enter(Hold::DeviceCreate).held);
    bool refused = false;
    std::thread other(
        [&]
        {
            TryScope attempt(Hold::Ngx, gate);
            refused = !attempt.Held();
        });
    other.join(); // if TryEnter accidentally waits, it times out and the timeout assertion below fails
    assert(refused && gate.Snapshot().timedOut == 0);
    {
        TryScope recursive(Hold::Ngx, gate);
        assert(recursive.Held());
    }
    assert(gate.HeldHere());
    gate.Leave(Hold::DeviceCreate);
    {
        TryScope admitted(Hold::Ngx, gate);
        assert(admitted.Held() && gate.HeldHere());
    }
    assert(!gate.HeldHere());
}
