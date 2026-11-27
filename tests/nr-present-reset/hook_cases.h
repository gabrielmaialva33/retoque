int main()
{
    auto& p = DlssNr::PresentReset::Instance();
    sl::Constants c;
    assert(StreamlineHooks::hkslSetConstants(c, 1, 0) == sl::eError);
    assert(forwarded == 1 && fgCalls == 0 && p.Snapshot() == 0); // eInvalid is not true
    c.reset = sl::eTrue;
    Config::Instance()->DlssNrEnabled.value = false;
    StreamlineHooks::hkslSetConstants(c, 2, 0);
    assert(p.Snapshot() == 0 && forwarded == 2 && fgCalls == 0);
    Config::Instance()->DlssNrEnabled.value = true;
    StreamlineHooks::hkslSetConstants(c, 3, 0);
    auto cut = p.Snapshot();
    assert(cut != 0 && forwarded == 3 && fgCalls == 0); // native game FG, no OptiScaler FG input
    c.reset = sl::eFalse;
    StreamlineHooks::hkslSetConstants(c, 4, 0);
    assert(p.Snapshot() == cut); // false does not clear an unconsumed reset
    p.Confirm(cut, true, true);
    for (auto fg : { FGInput::DLSSG, FGInput::NvngxFG })
    {
        State::Instance().activeFgInput = fg;
        StreamlineHooks::hkslSetConstants(c, 5, 0);
    }
    assert(fgCalls == 2 && forwarded == 6);
    State::Instance().activeFgInput = FGInput::Other;
    c.reset = sl::eTrue;
    assert(!StreamlineHooks::hkslSetConstants_sl1(c, 6, 0));
    assert(p.Snapshot() != 0 && forwarded == 7 && fgCalls == 2);
    p.Confirm(p.Snapshot(), true, true);
    c.reset = sl::eInvalid;
    StreamlineHooks::hkslSetConstants_sl1(c, 7, 0);
    assert(p.Snapshot() == 0 && forwarded == 8 && fgCalls == 2);
    State::Instance().activeFgInput = FGInput::DLSSG;
    StreamlineHooks::hkslSetConstants_sl1(c, 8, 0);
    assert(fgCalls == 3 && forwarded == 9);
}
