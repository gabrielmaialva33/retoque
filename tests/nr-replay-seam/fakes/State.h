#pragma once
#include <cstdint>
struct State
{
    uint64_t frameCount = 0;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
