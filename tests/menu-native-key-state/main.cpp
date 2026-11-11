
static int failures = 0;

static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static bool Down(int vk) { return (OptiInput::RealGetAsyncKeyStateSafe(vk) & 0x8000) != 0; }

int main(int argc, char** argv)
{
    const std::string name = argc == 2 ? argv[1] : "";

    if (name == "native")
    {
        fake::held[0x2D] = true; // VK_INSERT, the default menu key
        fake::held[0x01] = true; // VK_LBUTTON
        Check(GetAsyncKeyState(0x2D) == 0, "control: the overlay's detour reports the held menu key as up");
        Check(Down(0x2D), "the menu key held under an overlay that swallows it is read as down");
        Check(Down(0x01), "a mouse button too");
        Check(Down(0x2D), "and stays down while held");
        fake::held[0x2D] = false;
        Check(!Down(0x2D), "its release is read");
        Check(fake::chainCalls == 1, "the menu's reads do not go through the user32 detour chain");
        const int native = fake::nativeCalls;
        Check(!Down(-1) && !Down(256), "keys out of range are up");
        Check(fake::nativeCalls == native, "and never reach the system call");
        // A tap between two of the game's reads: the menu's read takes the press bit, the game's next read gets it back
        fake::pressedSinceLastRead[0x20] = true; // VK_SPACE
        Check((OptiInput::RealGetAsyncKeyStateSafe(0x20) & 1) != 0, "the menu's native read takes the press bit");
        Check((OptiInput::WithTakenAsyncKeyPress(0x20, 0, false) & 1) != 0, "the game's next read gets it back");
        Check((OptiInput::WithTakenAsyncKeyPress(0x20, 0, false) & 1) == 0, "once");
    }
    else if (name == "missing")
    {
        // win32u without the export (Windows before 10 1607): the user32 path as before, under the hook bypass
        fake::win32uExports = false;
        fake::held[0x2D] = true;
        Check(!Down(0x2D), "without the export the read goes through the chain as before");
        Check(fake::nativeCalls == 0, "and never calls a missing export");
        Check(fake::chainCalls == 1 && fake::bypassed == 1, "it calls the trampoline with our own detour bypassed");
        OptiInput::o_GetAsyncKeyState = nullptr;
        Check(!Down(0x2D) && fake::chainCalls == 2, "without a trampoline it calls GetAsyncKeyState itself");
    }
    else
    {
        std::printf("unknown case\n");
        return 2;
    }

    std::printf(failures == 0 ? "PASS: %s\n" : "FAIL: %s (%d)\n", name.c_str(), failures);
    return failures == 0 ? 0 : 1;
}
