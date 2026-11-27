#include "pch.h"
#include "../dlssnr-loopback/mask_matrix.h"
#include <cassert>
#include <iostream>

struct Resource
{
};
struct Parameters
{
    unsigned long long integer = 999;
    Resource* pointer = nullptr;
    bool badRead = false;
    void Set(const char*, unsigned long long value) { integer = value; }
    void Set(const char*, Resource* value) { pointer = value; }
    unsigned Get(const char*, unsigned long long* value)
    {
        *value = integer;
        return badRead ? 0 : 1;
    }
    unsigned Get(const char*, Resource** value)
    {
        *value = pointer;
        return badRead ? 0 : 1;
    }
};

int main()
{
    Resource resource;
    Parameters params;
    assert(MaskMatrix::SetResource(params, &resource, true));
    assert(params.pointer == &resource && params.integer == 0);
    assert(MaskMatrix::SetResource(params, &resource, false));
    assert(params.pointer == nullptr && params.integer == reinterpret_cast<uintptr_t>(&resource));
    assert(MaskMatrix::SetResource(params, static_cast<Resource*>(nullptr), true));
    assert(params.pointer == nullptr && params.integer == 0);
    params.badRead = true;
    assert(!MaskMatrix::SetResource(params, &resource, false));
    assert(!MaskMatrix::SetResource(params, &resource, true));
    for (const auto& trial : MaskMatrix::Trials())
    {
        std::cout << trial.name << '\t' << trial.masked << '\t' << trial.r8 << '\t' << trial.resourceSetter << '\t'
                  << trial.blend;
        for (auto value : trial.rgba)
            std::cout << '\t' << unsigned(value);
        std::cout << '\n';
    }
}
