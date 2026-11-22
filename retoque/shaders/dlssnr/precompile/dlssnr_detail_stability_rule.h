// The per-pixel rule of DLSS-NR's detail stability: a temporal filter on the model's EDIT in the log domain, with the
// history clipped to the current 3x3 and rejected where the input moved under it. docs/dlssnr/design/detail-stability.md
// has the reasoning and says how this differs from the dead end recorded in retoque/dlssnr/README.md. Two things run
// this file: the pass (dlssnr_detail_stability.hlsl beside it) and the host test tests/nr-detail-stability, so the test runs
// the shader's own decisions rather than a copy of them.
//
// The filter is DLSS5-NR-Nexus's "Detail stability" temporal filter (williamstosh, MIT, shaders/look_compose.cs_5_1.hlsl at
// 8fabd388e9; Licenses/NRNexus_LICENSE.txt): a history of the log edit, reprojected with the game's motion vectors, clipped
// to the 3x3 range of the current edit, rejected where the reprojected input falls outside the 3x3 range of the current
// input, blended at alpha (0.15) per frame. The bands that decide the rejection are near binary and judged in c / (1 + c),
// the A-ENTROPY note (GPL-3.0, magpie-dlss5-amd, "avoid wide smoothstep weights that wobble"); the exact repeat is
// DLSS5-Feeder's RESTORE_HOLD_REPEATS (jlrouzies-fr, MIT, shaders/DLSS5_Feed.fx at 9bd0af3d). The code is ours: this fork
// edits the encoded proxy and answer per channel instead of Nexus's banded luminance and chroma, and keeps the input in
// one channel of its history instead of the whole witness.
//
// Plain C/HLSL subset: no templates, no references, no texture types, no swizzles beyond .x .y .z. Positions are in
// working pixels with texel INDICES as sample positions (a bilinear fetch at an integer position is that texel), and a
// motion vector points from this frame back to the last one (q = p + v), as the game's DLSS vectors do.
//
// The includer defines, before including this file:
//   DS_FN                function qualifier (nothing in HLSL, `inline` in C++)
//   DS_UNROLL            loop hint (`[unroll]` in HLSL, nothing in C++)
//   DS_PROXY(tx, ty)     this frame's proxy (what the model was shown) at a working texel, clamped    -> float3
//   DS_ANSWER(tx, ty)    this frame's answer (what the model returned) at a working texel, clamped      -> float3
//   DS_MOTION(tx, ty)    this frame's motion in working pixels, this frame -> the last one              -> float2
//   DS_HISTORY(tx, ty)   last frame's history at a working texel, clamped: .xyz the filtered edit, .w the input -> float4
//   DS_UI(tx, ty)        this frame's UI alpha, 0..1 (read only when haveUi is set)                     -> float
//   DS_ROUND16(v)        a float rounded to the half the history is stored in                           -> float
// In C++ also float2, float3, float4 (members x, y, z, w; arithmetic with each other and with float) and abs, min, max,
// clamp, floor, saturate, smoothstep, log2, exp2 and isfinite for floats. Name macro parameters anything but x and y.

#ifndef DLSSNR_DETAIL_STABILITY_RULE_H
#define DLSSNR_DETAIL_STABILITY_RULE_H

struct DsParams
{
    int width; // the model's working size
    int height;
    float alpha;    // weight of the current frame per frame (1 is no filter): 0.15
    int holdRepeats; // an input shown exactly as before is shown exactly as before
    int historyValid; // 0 after a reset, a gap, a size change: nothing to filter with
    int haveUi;       // 1 when DS_UI is this frame's UI alpha
    float bandLow;    // the rejection band, in c / (1 + c) of the input's luminance: none below, whole above
    float bandHigh;
};

// The edit's offset: the log ratio is taken on (value + k), so a pixel near black does not turn a mottled grey of a few
// 8-bit steps into a ratio of thousands. 1/256 is a step of an 8-bit encoded frame.
#define DS_K (1.0f / 256.0f)

struct DsResult
{
    float3 answer;  // what the resolve is shown at this pixel
    float4 history; // what is stored for the next frame: the filtered edit and this frame's input
    float weight;   // the blend weight of the current frame that was used, 1 is the model's answer as it is
};

DS_FN float3 DsMin3(float3 a, float3 b) { return float3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)); }

DS_FN float3 DsMax3(float3 a, float3 b) { return float3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }

DS_FN bool DsFinite3(float3 v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }

DS_FN float3 DsRound3(float3 v) { return float3(DS_ROUND16(v.x), DS_ROUND16(v.y), DS_ROUND16(v.z)); }

// The model's edit at one pixel: log2((answer + k) / (proxy + k)) per channel, on the non-negative part of both.
DS_FN float3 DsEdit(float3 answer, float3 proxy)
{
    return float3(log2((max(answer.x, 0.0f) + DS_K) / (max(proxy.x, 0.0f) + DS_K)),
                  log2((max(answer.y, 0.0f) + DS_K) / (max(proxy.y, 0.0f) + DS_K)),
                  log2((max(answer.z, 0.0f) + DS_K) / (max(proxy.z, 0.0f) + DS_K)));
}

// The answer an edit makes of a proxy.
DS_FN float3 DsReconstruct(float3 proxy, float3 edit)
{
    return float3(max((max(proxy.x, 0.0f) + DS_K) * exp2(edit.x) - DS_K, 0.0f),
                  max((max(proxy.y, 0.0f) + DS_K) * exp2(edit.y) - DS_K, 0.0f),
                  max((max(proxy.z, 0.0f) + DS_K) * exp2(edit.z) - DS_K, 0.0f));
}

// The input signal the rejection is judged on: Rec.709 luminance of the proxy in c / (1 + c), which is bounded and
// does not give a bright pixel's small change the room a plain difference would.
DS_FN float DsSignal(float3 proxy)
{
    const float y = max(0.2126f * proxy.x + 0.7152f * proxy.y + 0.0722f * proxy.z, 0.0f);
    return DS_ROUND16(y / (1.0f + y));
}

// The filtered edit and the answer it makes at one pixel, with the history for the next frame.
//
//   current   edit e, input s (this pixel), and the 3x3 around it: lo/hi of e per channel, slo/shi of s
//   history   h (edit, input) at q = p + motion, bilinear; the pixel is REJECTED where q is off the picture, where any of
//             the four texels holds nothing, or where the reprojected input hs lies outside [slo, shi] by more than
//             bandLow, whole at bandHigh. Rejected is the model's answer as it is.
//   blend     beta = max(alpha, rejection); the history is clipped to [lo, hi] and the edit is clipped + beta * (e - clipped)
//   repeat    holdRepeats, a motion of exactly zero and an input exactly the history's: the history's edit, with no clip and
//             no blend -- a repeated input frame shown exactly as it was
//   interface a pixel the mask takes for interface shows the answer as it is and stores its own edit; between, the edit
//             is blended toward the model's by the mask, and shown from that
//   store     the edit rounded to the half it is stored in -- and shown from that, so a repeat is bit for bit
DS_FN DsResult DsFilter(int px, int py, DsParams P)
{
    DsResult r;
    const float3 proxy = DS_PROXY(px, py);
    const float3 answerNow = DS_ANSWER(px, py);
    r.answer = answerNow;
    r.history = float4(0.0f, 0.0f, 0.0f, -1.0f); // an input below zero is nothing stored
    r.weight = 1.0f;

    // Nothing finite to filter: the answer as it is, and nothing stored for the next frame to read.
    if (!DsFinite3(proxy) || !DsFinite3(answerNow))
        return r;

    const float3 e = DsRound3(DsEdit(answerNow, proxy));
    const float s = DsSignal(proxy);
    r.history = float4(e.x, e.y, e.z, s);
    if (P.historyValid == 0 || P.alpha >= 1.0f)
        return r;

    const float ui = P.haveUi != 0 ? saturate(DS_UI(px, py)) : 0.0f;
    if (ui >= 1.0f)
        return r;

    // The game's vector, to where this pixel was last frame.
    const float2 motion = DS_MOTION(px, py);
    if (!isfinite(motion.x) || !isfinite(motion.y))
        return r;
    const float qx = (float) px + motion.x;
    const float qy = (float) py + motion.y;
    const float limitX = (float) P.width - 0.5f;
    const float limitY = (float) P.height - 0.5f;
    if (!(qx >= -0.5f && qx <= limitX && qy >= -0.5f && qy <= limitY))
        return r;

    const float posX = clamp(qx, 0.0f, (float) (P.width - 1));
    const float posY = clamp(qy, 0.0f, (float) (P.height - 1));
    const int hx0 = (int) floor(posX);
    const int hy0 = (int) floor(posY);
    const int hx1 = min(hx0 + 1, P.width - 1);
    const int hy1 = min(hy0 + 1, P.height - 1);
    const float fx = posX - (float) hx0;
    const float fy = posY - (float) hy0;
    const float4 h00 = DS_HISTORY(hx0, hy0);
    const float4 h10 = DS_HISTORY(hx1, hy0);
    const float4 h01 = DS_HISTORY(hx0, hy1);
    const float4 h11 = DS_HISTORY(hx1, hy1);
    // A texel that stored nothing (a pixel that was not finite, a frame the pass did not filter) leaves nothing to filter
    // with where any of the four weights reaches it.
    if ((h00.w < 0.0f && (1.0f - fx) * (1.0f - fy) > 0.0f) || (h10.w < 0.0f && fx * (1.0f - fy) > 0.0f) ||
        (h01.w < 0.0f && (1.0f - fx) * fy > 0.0f) || (h11.w < 0.0f && fx * fy > 0.0f))
        return r;
    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 = fx * (1.0f - fy);
    const float w01 = (1.0f - fx) * fy;
    const float w11 = fx * fy;
    const float3 he = float3(h00.x * w00 + h10.x * w10 + h01.x * w01 + h11.x * w11,
                             h00.y * w00 + h10.y * w10 + h01.y * w01 + h11.y * w11,
                             h00.z * w00 + h10.z * w10 + h01.z * w01 + h11.z * w11);
    const float hs = h00.w * w00 + h10.w * w10 + h01.w * w01 + h11.w * w11;

    // The current 3x3: the edit's range, and the input's.
    float3 lo = e;
    float3 hi = e;
    float slo = s;
    float shi = s;
    DS_UNROLL for (int dy = -1; dy <= 1; ++dy)
    {
        DS_UNROLL for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
                continue;
            const int nx = clamp(px + dx, 0, P.width - 1);
            const int ny = clamp(py + dy, 0, P.height - 1);
            const float3 nproxy = DS_PROXY(nx, ny);
            const float3 nanswer = DS_ANSWER(nx, ny);
            if (!DsFinite3(nproxy) || !DsFinite3(nanswer))
                continue;
            const float3 ne = DsRound3(DsEdit(nanswer, nproxy));
            const float ns = DsSignal(nproxy);
            lo = DsMin3(lo, ne);
            hi = DsMax3(hi, ne);
            slo = min(slo, ns);
            shi = max(shi, ns);
        }
    }

    // The input under the history changed: where the reprojected input leaves the current 3x3 range of it. Near binary:
    // nothing under bandLow, everything over bandHigh.
    const float outside = max(max(slo - hs, hs - shi), 0.0f);
    const float rejection = smoothstep(P.bandLow, P.bandHigh, outside);
    float beta = max(P.alpha, rejection);

    float3 filtered;
    if (P.holdRepeats != 0 && motion.x == 0.0f && motion.y == 0.0f && hs == s && rejection <= 0.0f)
    {
        // An input shown before, exactly: the model's new opinion of it is not let in.
        filtered = float3(h00.x, h00.y, h00.z);
        beta = 0.0f;
    }
    else if (beta >= 1.0f)
    {
        filtered = e;
        beta = 1.0f;
    }
    else
    {
        const float3 clipped = DsMin3(DsMax3(he, lo), hi);
        filtered = clipped + (e - clipped) * beta;
    }

    // The interface takes the model's answer; what is stored is the edit that made it.
    filtered = filtered + (e - filtered) * ui;
    filtered = DsRound3(filtered);
    r.history = float4(filtered.x, filtered.y, filtered.z, s);
    r.weight = beta;
    if (beta >= 1.0f || ui >= 1.0f)
        return r;
    r.answer = DsReconstruct(proxy, filtered);
    return r;
}

#endif
