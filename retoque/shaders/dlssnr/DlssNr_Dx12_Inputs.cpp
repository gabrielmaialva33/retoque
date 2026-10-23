#include "pch.h"

// What the game's parameter block says about a frame, read the same way by every route that has one: after the
// upscaler, before it, and the present-time hosts given a guide frame. Moved whole from DlssNr_Dx12.cpp (split step 3,
// docs/dlssnr/design/dx12-internal.md); the bodies and their function-local reporters are unchanged.

#include "DlssNr_Dx12_Internal.h"

#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNr_Exposure.h>
#include <dlssnr/DlssNr_ExposureScan.h>
#include <dlssnr/DlssNr_LogRate.h>
#include <dlssnr/DlssNr_RenderSize.h>
#include <dlssnr/DlssNr_WorkingScale.h>

#include <Config.h>
#include <Logger.h>

#include <cmath>
#include <mutex>
#include <string>

using namespace DlssNr::Dx12Internal;

namespace DlssNr
{
// What the parameter block says about this frame, read the same way whichever side of the upscaler
// the model runs on. Everything here is a property of how the game encodes its buffers, and both
// paths read the block the game filled for the upscaler.
// Diagnostic-only contract: allocation size is not evidence of the rendered subrect.
void ReportSpatialContract(NVSDK_NGX_Parameter* params, ID3D12Resource* color, ID3D12Resource* output, bool before,
                           unsigned int inputWidth, unsigned int inputHeight)
{
    struct Contract
    {
        uint64_t colorWidth = 0, outputWidth = 0;
        unsigned colorHeight = 0, outputHeight = 0, subWidth = 0, subHeight = 0, modelWidth = 0, modelHeight = 0;
        bool haveWidth = false, haveHeight = false;
        unsigned resolvedWidth = 0, resolvedHeight = 0;
        DlssNr::RenderSizeSource source = DlssNr::RenderSizeSource::Missing;
        float scale = 1.0f;
        bool operator==(const Contract&) const = default;
    } now;
    if (color)
    {
        const auto desc = color->GetDesc();
        now.colorWidth = desc.Width;
        now.colorHeight = desc.Height;
    }
    if (output)
    {
        const auto desc = output->GetDesc();
        now.outputWidth = desc.Width;
        now.outputHeight = desc.Height;
    }
    now.haveWidth = params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &now.subWidth) ==
                    NVSDK_NGX_Result_Success;
    now.haveHeight = params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &now.subHeight) ==
                     NVSDK_NGX_Result_Success;
    const auto resolved = DlssNr::ResolveRenderSize(params);
    now.resolvedWidth = resolved.size.width;
    now.resolvedHeight = resolved.size.height;
    now.source = resolved.source;
    now.scale = DlssNr::WorkingScale();
    // Match Dispatch's clamping for logging only; never feed diagnostic values back to rendering.
    if (std::isfinite(now.scale))
    {
        now.scale = std::clamp(now.scale, 0.25f, 2.0f);
        now.modelWidth = static_cast<unsigned>(inputWidth * now.scale + 0.5f);
        now.modelHeight = static_cast<unsigned>(inputHeight * now.scale + 0.5f);
    }
    else
        now.scale = 0;
    static std::mutex mutex;
    static Contract previous[2];
    static bool seen[2] {};
    std::lock_guard<std::mutex> lock(mutex);
    const unsigned stage = before ? 1 : 0;
    if (seen[stage] && previous[stage] == now)
        return;
    seen[stage] = true;
    previous[stage] = now;
    LOG_INFO("DLSS-NR spatial contract stage={}: Color-allocation={}x{}, render-subrect={}x{} (width-query={}, "
             "height-query={}), "
             "game-output={}x{}, model-request={}x{}, WorkingScale={:.3f}, render-resolved={}x{} source={}",
             before ? "before" : "after", now.colorWidth, now.colorHeight, now.subWidth, now.subHeight,
             now.haveWidth ? "success" : "absent/failed", now.haveHeight ? "success" : "absent/failed", now.outputWidth,
             now.outputHeight, now.modelWidth, now.modelHeight, now.scale, now.resolvedWidth, now.resolvedHeight,
             DlssNr::RenderSizeSourceName(now.source));
    if (before && now.outputWidth && now.outputHeight && now.modelWidth >= now.outputWidth &&
        now.modelHeight >= now.outputHeight)
        LOG_INFO("DLSS-NR spatial contract: no spatial reduction before the upscaler; this does not establish DLAA. "
                 "Check render-subrect query presence and WorkingScale; Color allocation alone cannot establish "
                 "rendered resolution");
}

DlssNrFrameInfo GatherGuideFrame(NVSDK_NGX_Parameter* params)
{
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

    DlssNrFrameInfo frame {};
    // Depth and motion come from the game's upscaler evaluate: real guides, which model cadence needs.
    frame.GameGuides = true;
    frame.DepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.ColourIsLinearHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    frame.MotionVectorsLowResolution = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;

    // The game telling the upscaler to forget everything it has accumulated: a cut, a teleport, a
    // load. Every upscaler in this tree reads it and this pass did not, so the model's history was
    // only ever reset by things that happened to us -- a resize, a rebuild, a recovery from failure
    // -- and never by anything that happened in the game. Across a cut the model was reprojecting
    // the previous scene onto the new one and being asked to reconcile them.
    //
    // Read the same way FFXFeature_Dx12 reads it, including leaving it alone when the parameter is
    // absent: a game that never sets it is not asking for a reset every frame.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success)
            frame.Reset = gameReset != 0;
    }

    // How much of the guides is real. Unknown/incomplete game geometry is refused by Dispatch.
    const auto renderSize = DlssNr::GetRenderSize(params);
    frame.RenderSubrectWidth = renderSize.width;
    frame.RenderSubrectHeight = renderSize.height;
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.DepthSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.DepthSubrectBaseY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.MotionSubrectBaseY);

    // A scale that was set but is zero or not finite is as good as absent: handed to the model it
    // collapses every vector to nothing or poisons them with NaN.
    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.MvScaleX) || frame.MvScaleX == 0.0f)
        frame.MvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) != NVSDK_NGX_Result_Success ||
        !std::isfinite(frame.MvScaleY) || frame.MvScaleY == 0.0f)
        frame.MvScaleY = 1.0f;

    return frame;
}

DlssNrFrameInfo GatherFrame(NVSDK_NGX_Parameter* params)
{
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

    DlssNrFrameInfo frame = GatherGuideFrame(params);

    // What the game says about its own exposure. Logged, used for nothing yet.
    //
    // The white point measured from the frame turned out to be a control loop rather than a
    // measurement: the pass writes into the buffer it reads, most games adapt their exposure to the
    // finished frame, and the two chase each other -- 0.01 to 97.9 in one Enshrouded session. Any
    // statistic taken from a frame we modify has that problem.
    //
    // These do not. DLSS.Pre.Exposure is the scale the game applied before handing the buffer over,
    // and ExposureTexture is a 1x1 the game fills with the exposure it is using; both are the game's
    // own numbers, decided upstream of anything here. Whether either is close to the divisor the model
    // actually wants is unknown, which is why this only prints them.
    //
    // The auto-exposure flag decides whether the texture means anything: with it set the game is
    // telling DLSS to work exposure out for itself and may supply nothing. OptiScaler forces that flag
    // on for eighteen games, so it is logged too -- reading a value whose flag has been overridden is
    // how the debug views lied earlier tonight.
    {
        float preExposure = 0.0f;
        const bool havePre =
            params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

        DlssNr::Detail::ColorBinding<NVSDK_NGX_Parameter, ID3D12Resource> exposureBinding;
        auto* exposureTex = exposureBinding.Read(params, NVSDK_NGX_Parameter_ExposureTexture, NVSDK_NGX_Result_Success);

        frame.ExposureTexture = exposureTex;
        frame.PreExposure = havePre ? DlssNr::Exposure::PreExposure(preExposure) : 1.0f;

        g_nr.exposureOfferedNow = exposureTex != nullptr;
        g_nr.exposureEverOffered = g_nr.exposureEverOffered || g_nr.exposureOfferedNow;
        g_nr.exposureFrames++;

        const bool autoExposureFlag = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;

        struct ExposureReport
        {
            float pre;
            bool havePre;
            bool haveTexture;
            bool autoFlag;

            bool SameAs(const ExposureReport& other) const
            {
                return SameShape(other) && std::abs(pre - other.pre) <= std::max(0.01f * std::abs(pre), 1e-4f);
            }

            // What the game supplies, as opposed to how much of it. A change here is the game
            // changing its mind, which is worth a line the moment it happens.
            bool SameShape(const ExposureReport& other) const
            {
                return havePre == other.havePre && haveTexture == other.haveTexture && autoFlag == other.autoFlag;
            }
        };

        static LogRate::Reporter<ExposureReport> exposureReporter;
        const ExposureReport now { havePre ? preExposure : 0.0f, havePre, exposureTex != nullptr, autoExposureFlag };

        if (exposureReporter.Observe(now, LogRate::Clock::now()))
        {
            LOG_INFO("DLSS-NR exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}, "
                     "auto-exposure flag {}",
                     now.havePre ? std::to_string(now.pre) : std::string("not supplied"),
                     now.haveTexture ? "supplied" : "not supplied", now.autoFlag ? "set" : "clear");
        }

        // The value itself, once it has come back off the GPU. Separate from the line above because
        // that one says what the game offers and this one says what it actually reads -- and because
        // the reading arrives three frames after the offer.
        struct ExposureValueReport
        {
            LogRate::Drift exposure;
            LogRate::Drift pre;
            bool SameShape(const ExposureValueReport&) const { return true; }
            bool SameAs(const ExposureValueReport& other) const
            {
                return exposure.SameAs(other.exposure) && pre.SameAs(other.pre);
            }
        };
        static LogRate::Reporter<ExposureValueReport> exposureValue;
        const ExposureValueReport exposureNow { { g_nr.gameExposure, 0.02f, 1e-5f },
                                                { g_nr.gamePreExposure, 0.01f, 1e-4f } };

        if (g_nr.gameExposure > 1e-6f && exposureValue.Observe(exposureNow, LogRate::Clock::now()))
        {
            LOG_INFO("DLSS-NR game exposure {:.5f} (pre-exposure {:.3f}) -> white point would be {:.2f}",
                     g_nr.gameExposure, g_nr.gamePreExposure, g_nr.gamePreExposure / g_nr.gameExposure);
        }

        // The scan's number, on the same cadence, so one log carries both.
        //
        // This is the whole validation. In a game that hands over an exposure texture there is a
        // known-correct value; if the scan's candidate tracks it, the scan found the right buffer
        // rather than merely a moving one, and can be trusted where a game hands over nothing.
        // Comparing two numbers after the fact needs both written down, and until now the scan's
        // value existed only in a menu nobody can read while playing.
        {
            int which = 0;
            float low = 0.0f, high = 0.0f;
            const float scanned = DlssNr::ExposureScan::BestValue(&which, &low, &high);

            struct ScanReport
            {
                LogRate::Drift value, low, high, game;
                int candidate;
                bool scanning;
                bool haveGame;
                bool SameShape(const ScanReport& other) const
                {
                    return candidate == other.candidate && scanning == other.scanning && haveGame == other.haveGame;
                }
                bool SameAs(const ScanReport& other) const
                {
                    return SameShape(other) && value.SameAs(other.value) && low.SameAs(other.low) &&
                           high.SameAs(other.high) && game.SameAs(other.game);
                }
            };
            static LogRate::Reporter<ScanReport> scanValue;
            const ScanReport scanNow { { scanned, 0.02f, 1e-6f },
                                       { low, 0.02f, 1e-6f },
                                       { high, 0.02f, 1e-6f },
                                       { g_nr.gameExposure, 0.02f, 1e-5f },
                                       which,
                                       ExposureScan::Scanning(),
                                       g_nr.gameExposure > 1e-6f };

            if (scanned > 0.0f && scanValue.Observe(scanNow, LogRate::Clock::now()))
            {
                if (g_nr.gameExposure > 1e-6f)
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  the "
                             "game's own exposure is {:.5f}  |  ratio {:.4f}",
                             which, scanned, low, high, g_nr.gameExposure, scanned / g_nr.gameExposure);
                else
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  this "
                             "game supplies no exposure to compare against",
                             which, scanned, low, high);
            }
        }
    }

    return frame;
}

int GuideRestState(bool motionVectors)
{
    const Config& cfg = *Config::Instance();
    const auto value = motionVectors ? cfg.MVResourceBarrier : cfg.DepthResourceBarrier;
    return value.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
} // namespace DlssNr
