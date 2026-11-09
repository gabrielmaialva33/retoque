#include "pch.h"

int main(int argc, char** argv)
{
    require(argc == 2, "one case required");
    const auto is = [&](const char* name) { return std::strcmp(argv[1], name) == 0; };
    if (is("nested"))
    {
        require(!State::Instance().vulkanCreatingSC, "initial scope is set");
        {
            ScopedVulkanCreatingSC outer;
            require(State::Instance().vulkanCreatingSC, "outer scope is clear");
            {
                ScopedVulkanCreatingSC inner;
                require(State::Instance().vulkanCreatingSC, "inner scope is clear");
            }
            require(State::Instance().vulkanCreatingSC, "inner exit cleared outer scope");
        }
        require(!State::Instance().vulkanCreatingSC, "outer exit left scope set");
    }
    else if (is("interleaved"))
    {
        // Ordered semaphores avoid data races even with the deliberately process-global mutant.
        std::binary_semaphore entered(0), checked(0), exited(0), secondDone(0);
        std::thread presenter(
            [&]
            {
                {
                    ScopedVulkanCreatingSC outer;
                    entered.release();
                    checked.acquire();
                }
                exited.release();
                secondDone.acquire();
                require(!State::Instance().vulkanCreatingSC, "other exit contaminated presenter");
            });
        entered.acquire();
        require(!State::Instance().vulkanCreatingSC, "other thread inherited Vulkan scope");
        {
            ScopedVulkanCreatingSC gameReentry;
            checked.release();
            exited.acquire();
            require(State::Instance().vulkanCreatingSC, "other thread exit cleared game scope");
        }
        require(!State::Instance().vulkanCreatingSC, "game exit left scope set");
        secondDone.release();
        presenter.join();
    }
    else if (is("dxgi") || is("native"))
    {
        const bool dxgi = is("dxgi");
        for (int i = 0; i < 3; ++i)
            presentUpdate(dxgi);
        require(ReflexHooks::updates == (dxgi ? 0 : 3),
                dxgi ? "DXGI-backed present updated Reflex again" : "native Vulkan lost Reflex update");
    }
    else if (is("never-async"))
    {
        for (uint64_t id : { 1u, 11u, 500u })
        {
            Marker marker { id };
            processMarker(&marker);
            require(_FgNumFramesToGenerate == 3, "game without async markers lost FG count");
        }
    }
    else if (is("rtss"))
    {
        _lastAsyncMarkerFrameId = 100;
        Marker marker { (uint64_t { 1 } << 32) | 500 };
        processMarker(&marker);
        require(_FgNumFramesToGenerate == 3, "RTSS marker expired game async markers");
    }
    else if (is("expiry"))
    {
        _lastAsyncMarkerFrameId = 100;
        for (uint64_t id : { 99u, 100u, 110u, 111u })
        {
            _FgNumFramesToGenerate = 3;
            Marker marker { id };
            processMarker(&marker);
            require(_FgNumFramesToGenerate == (id > 110 ? 0u : 3u), "ten-frame async grace changed");
        }
    }
    else
        require(false, "unknown case");
}
