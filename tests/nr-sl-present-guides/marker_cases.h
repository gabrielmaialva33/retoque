int main()
{
    using sl::PCLMarker;
    using sl::Result;
    assert(StreamlineHooks::hkslPCLSetMarker(PCLMarker::ePresentStart, 42) == Result::eOk);
    assert(forwarded == 1 && starts == 0);
    enabled = true;
    assert(StreamlineHooks::hkslPCLSetMarker(PCLMarker::ePresentStart, 42) == Result::eOk);
    assert(forwarded == 2 && starts == 1 && startedToken == 42);
    originalResult = Result::eError;
    assert(StreamlineHooks::hkslPCLSetMarker(PCLMarker::ePresentStart, 43) == Result::eError);
    assert(forwarded == 3 && starts == 1 && ends == 1);
    originalResult = Result::eOk;
    renderApi = sl::RenderAPI::eVulkan;
    StreamlineHooks::hkslPCLSetMarker(PCLMarker::ePresentStart, 44);
    assert(starts == 1 && ends == 2);
    renderApi = sl::RenderAPI::eD3D12;
    State::Instance().streamlineVersion.major = 1;
    StreamlineHooks::hkslPCLSetMarker(PCLMarker::ePresentStart, 44);
    assert(starts == 1 && ends == 3);
    State::Instance().streamlineVersion.major = 2;
    State::Instance().activeFgInput = FGInput::DLSSG;
    StreamlineHooks::hkslPCLSetMarker(PCLMarker::ePresentStart, 45);
    assert(starts == 2 && State::Instance().slFGInputs.marks == 1);
    StreamlineHooks::hkslPCLSetMarker(PCLMarker::ePresentEnd, 45);
    assert(forwarded == 7 && ends == 4);
    o_slPCLSetMarker = nullptr;
    assert(StreamlineHooks::hkslPCLSetMarker(PCLMarker::ePresentStart, 46) == Result::eErrorFeatureMissing);
    assert(forwarded == 7 && starts == 2 && ends == 5);
    puts("PASS: actual PCL marker hook preserves forwarding/FG and observes only accepted SL2 D3D12 markers when "
         "enabled");
}
