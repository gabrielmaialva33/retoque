#pragma once
#include <dlssnr/DlssNr_PairCapture.h>
namespace DlssNr
{
struct PairCaptureStatus
{
    PairCapture::Phase phase = PairCapture::Phase::Idle;
    bool directoryFailed = false, gaveUp = false, ok = false;
    const char* reason = nullptr;
    std::string lastStem;
};
} // namespace DlssNr
