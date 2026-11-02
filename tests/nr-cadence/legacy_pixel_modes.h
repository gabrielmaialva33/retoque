// Frozen 73fad086 pixel modes; only the function names change to keep an independent probe oracle.
void LegacyPixelModes(uint3 id)
{
    const CadenceParams P = LoadParams();

    // Sparse adaptive feedback, including model frames: refusal must not latch by stopping its own probe.
    if (gMode == 9)
    {
        if (id.x >= 16 || id.y >= 16)
            return;
        const int x = int(((2 * id.x + 1) * gWidth) / 32);
        const int y = int(((2 * id.y + 1) * gHeight) / 32);
        const float d = DepthNowAt(x, y);
        const float2 motion = MotionAt(x, y);
        float4 sample = 0.0;
        if (!isfinite(d) || !CadenceFinite2(motion))
            sample.z = -1.0;
        else if (d > 0.0 && d < 1.0 && (gHaveUi == 0 || UiAt(x, y) < 0.5))
        {
            sample.z = -1.0;
            if (gProbeHistory != 0)
            {
                const float2 acc = CadenceChainStep(x, y, gChainStart != 0, P);
                const CadenceCarry carry = LegacyCarryEdit(x, y, acc, P, false);
                sample = float4(length(motion) * (1080.0 / float(gHeight)), 1.0 - carry.accept, 1.0, 0.0);
            }
        }
        gOut0[id.xy] = sample;
        return;
    }

    if (gMode == 1)
    {
        if (id.x >= gLowWidth || id.y >= gLowHeight)
            return;
        gOut0[id.xy] = float4(CadenceCoarse(int(id.x), int(id.y), P), 0.0);
        return;
    }

    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const int px = int(id.x);
    const int py = int(id.y);

    if (gMode == 0)
    {
        const float3 proxy = ProxyNowAt(px, py);
        gOut0[id.xy] = float4(CadenceResidual(gAnswer.Load(int3(id.xy, 0)).rgb, proxy), 0.0);
        gOut1[id.xy] = float4(proxy, 1.0);
        gOut2[id.xy] = DepthNowAt(px, py);
        return;
    }

    if (gMode == 2)
    {
        gOut0[id.xy] = float4(CadenceChainStep(px, py, gChainStart != 0, P), 0.0, 0.0);
        return;
    }

    if (gMode == 3)
    {
        // The alpha the model would return is the frame's own; the resolve reads colour only. At a weight of zero
        // this is CadenceSynthesize exactly, and the fade is not read.
        const float alpha = gProxy.Load(int3(id.xy, 0)).a;
        gOut0[id.xy] = float4(CadenceSynthesizeFaded(px, py, ChainAt(px, py), gFadeWeight, P), alpha);
        return;
    }

    if (gMode == 4)
    {
        const float2 acc = CadenceChainStep(px, py, false, P);
        gOut0[id.xy] = float4(CadenceModelMotion(px, py, acc, P), 0.0, 0.0);
        return;
    }

    if (gMode == 5)
    {
        // The same last link mode 4 computed for the model's vectors, from the same chain: the carried answer here
        // is what a carried frame would have shown.
        const float2 acc = CadenceChainStep(px, py, false, P);
        const float4 answer = gAnswer.Load(int3(id.xy, 0));
        const float3 fade = CadenceFade(px, py, acc, answer.rgb, P);
        gOut0[id.xy] = float4(CadenceFadeShown(answer.rgb, fade, gFadeWeight), answer.a);
        gOut1[id.xy] = float4(fade, 0.0);
        return;
    }

    if (gMode == 8)
    {
        // A pixel taken for interface keeps its answer as it is: the gain holds back the scenery's edit, not the HUD.
        const float4 shown = gAnswer.Load(int3(id.xy, 0));
        const float keep = gHaveUi != 0u ? 1.0 - saturate(UiAt(px, py)) : 1.0;
        gOut0[id.xy] = float4(CadenceApplyGain(shown.rgb, gOut2[uint2(3, 0)] * keep, int(gPassthrough)), shown.a);
        return;
    }
}
