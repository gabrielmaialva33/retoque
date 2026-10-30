
// ---- cases ----

static int failures = 0;
static void check(bool passed, const std::string& message)
{
    if (!passed)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

static int Present(IFGFeature_Dx12& fg)
{
    int present = 0;
    for (int i = 0; i < BUFFER_COUNT; ++i)
        present += (fg._uiCommandAllocator[i] != nullptr) + (fg._uiCommandList[i] != nullptr) +
                   (fg._scCommandAllocator[i] != nullptr) + (fg._scCommandList[i] != nullptr);
    return present;
}

static void Release(IFGFeature_Dx12& fg)
{
    for (int i = 0; i < BUFFER_COUNT; ++i)
    {
        SAFE_RELEASE(fg._uiCommandAllocator[i]);
        SAFE_RELEASE(fg._uiCommandList[i]);
        SAFE_RELEASE(fg._scCommandAllocator[i]);
        SAFE_RELEASE(fg._scCommandList[i]);
    }
    SAFE_RELEASE(fg._uiFence);
    SAFE_RELEASE(fg._scFence);
    SAFE_RELEASE(fg._gameCommandQueue);
}

// 01b95a7d97: a creation that fails part way is undone, so the next request creates the whole set again
template <typename Backend> void PartialCreation(const char* name)
{
    // A full set: per slot an allocator and a list for UI and for SC, plus the two shared fences once
    const int full = BUFFER_COUNT * 4 + 2;
    for (int failing = 1; failing <= full; ++failing)
    {
        const std::string label = std::string(name) + " creation failing at call " + std::to_string(failing);
        objectsAlive = 0;
        calls = 0;
        failAt = failing;
        ID3D12Device device;
        Backend fg;
        fg._device = &device;
        fg._gameCommandQueue = new ID3D12CommandQueue();
        fg.CreateObjects(&device);
        const bool fencesFailed = fg._uiFence == nullptr || fg._scFence == nullptr;
        check(Present(fg) == 0, label + ": a partial set is released, not kept as ready (" +
                                    std::to_string(Present(fg)) + " of " + std::to_string(BUFFER_COUNT * 4) + " left)");
        const int fences = (fg._uiFence != nullptr) + (fg._scFence != nullptr);
        check(objectsAlive == fences + 1,
              label + ": nothing but the shared fences (and the queue) outlives the failed attempt");
        (void) fencesFailed;

        // The next request on a healthy device builds the whole set
        failAt = -1;
        if (Present(fg) == 0)
        {
            for (int index = 0; index < BUFFER_COUNT; ++index)
            {
                fg.index = index;
                check(fg.GetUICommandList(index) != nullptr,
                      label + ": UI list available afterwards, slot " + std::to_string(index));
                check(fg.GetSCCommandList(index) != nullptr,
                      label + ": SC list available afterwards, slot " + std::to_string(index));
            }
            check(Present(fg) == BUFFER_COUNT * 4, label + ": the set is complete after the retry");
        }
        Release(fg);
        check(objectsAlive == 0, label + ": every object released exactly once");
    }
    failAt = -1;
}

// The getters never reset or hand out a slot that is missing, whatever left it so
template <typename Backend> void MissingSlot(const char* name)
{
    for (int missing = 0; missing < 4; ++missing)
    {
        objectsAlive = 0;
        calls = 0;
        failAt = -1;
        ID3D12Device device;
        Backend fg;
        fg._device = &device;
        fg._gameCommandQueue = new ID3D12CommandQueue();
        fg.CreateObjects(&device);
        const int slot = 1;
        ID3D12GraphicsCommandList** lists[] = { &fg._uiCommandList[slot], &fg._scCommandList[slot] };
        ID3D12CommandAllocator** allocators[] = { &fg._uiCommandAllocator[slot], &fg._scCommandAllocator[slot] };
        if (missing < 2)
            SAFE_RELEASE(*lists[missing])
        else
            SAFE_RELEASE(*allocators[missing - 2])
        const std::string label = std::string(name) + " getter with a missing " + (missing % 2 == 0 ? "UI " : "SC ") +
                                  (missing < 2 ? "list" : "allocator");
        // Only the guarded getters survive this: before 01b95a7d97 they called through the missing pointer
        fg.index = slot;
        if (missing % 2 == 0)
            check(fg.GetUICommandList(slot) == nullptr, label + ": refused");
        else
            check(fg.GetSCCommandList(slot) == nullptr, label + ": refused");
        Release(fg);
    }
}

// 1c90e142e8: a HUD-less format change restarts frame generation once, then the new format is accepted
template <typename Gate> void HudlessFormat(const char* name, Gate gate)
{
    FakeResource resource;
    FrameResource frame { &resource };
    auto& state = State::Instance();
    const std::string label = std::string(name) + " HUD-less format";
    for (int fIndex = 0; fIndex < BUFFER_COUNT; ++fIndex)
    {
        resource.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        state.fgChanged = false;
        check(gate(HudlessColor, &frame, fIndex), label + ": first format accepted");
        check(gate(HudlessColor, &frame, fIndex), label + ": same format accepted");
        check(!state.fgChanged, label + ": no restart without a change");

        resource.format = DXGI_FORMAT_R10G10B10A2_UNORM;
        check(!gate(HudlessColor, &frame, fIndex), label + ": a changed format is refused once");
        check(state.fgChanged, label + ": and frame generation restarts");
        state.fgChanged = false;
        for (int frameNumber = 0; frameNumber < 3; ++frameNumber)
        {
            check(gate(HudlessColor, &frame, fIndex),
                  label + ": the new format is accepted after the restart, frame " + std::to_string(frameNumber));
            check(!state.fgChanged, label + ": no restart every frame, frame " + std::to_string(frameNumber));
            state.fgChanged = false;
        }
    }
    // Other resources never pass through the gate
    resource.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    check(gate(Depth, &frame, 0) && !state.fgChanged, label + ": depth is not format-gated");
}

int main()
{
    HudlessFormat("DLSSG", HudlessGate_DLSSG);
    HudlessFormat("XeFG", HudlessGate_XeFG);
    PartialCreation<DLSSG_Dx12>("DLSSG");
    PartialCreation<XeFG_Dx12>("XeFG");
    MissingSlot<DLSSG_Dx12>("DLSSG");
    if (failures == 0)
        std::printf("fg-command-objects: all cases passed\n");
    return failures == 0 ? 0 : 1;
}
