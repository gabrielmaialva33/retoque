#pragma once

// Frozen 73fad086 carry, independent of the acceptance extraction under test.
// BeliyG3 attribution and licence: Licenses/OptimizerFps_LICENSE.txt.
inline CadenceCarry LegacyCarryEdit(int px, int py, float2 acc, CadenceParams P, bool stopWhenRejected)
{
    const float W = (float) P.width;
    const float H = (float) P.height;
    const float cx = (float) px + 0.5f;
    const float cy = (float) py + 0.5f;
    const float3 c = CR_PROXY_NOW(px, py);
    const float dc = CR_DEPTH_NOW(px, py);

    float2 d = acc;
    if (!CadenceFinite2(d))
        d = float2(0.0f, 0.0f);
    const float qx = cx + d.x;
    const float qy = cy + d.y;

    // Where the chain left the picture there is nothing to carry; near the edge the decision ramps, but only
    // where the chain moved q nearer the edge than the pixel is -- a still pixel by the border is carried
    // whole (BeliyG3 ramped every pixel within 16 px of the edge).
    const bool inside = qx >= 0.0f && qy >= 0.0f && qx < W && qy < H;
    const float distQ = min(min(qx, W - qx), min(qy, H - qy));
    const float distP = min(min(cx, W - cx), min(cy, H - cy));
    const float ramp = 16.0f * P.scale;
    const float wEdge = inside ? saturate(distQ / max(min(ramp, distP), 1e-3f)) : 0.0f;

    // Acceptance at the pixel and eight neighbours 3 px out, averaged, so a per-pixel decision does not draw
    // a line that jitters from frame to frame; then gated by the centre's own result, because the
    // neighbourhood softens a decision and must not overturn one (BeliyG3, 9-tap acceptance).
    float tapX[9] = { 0.0f, 3.0f, -3.0f, 0.0f, 0.0f, 3.0f, -3.0f, 3.0f, -3.0f };
    float tapY[9] = { 0.0f, 0.0f, 0.0f, 3.0f, -3.0f, 3.0f, 3.0f, -3.0f, -3.0f };
    float tapW[9] = { 0.28f, 0.12f, 0.12f, 0.12f, 0.12f, 0.06f, 0.06f, 0.06f, 0.06f };
    float w = 0.0f;
    float wTotal = 0.0f;
    float wCentre = 0.0f;
    CR_UNROLL for (int k = 0; k < 9; ++k)
    {
        const float ox = tapX[k] * P.scale;
        const float oy = tapY[k] * P.scale;
        const int pkx = CadenceTexel(clamp(cx + ox, 0.0f, W - 1.0f), P.width);
        const int pky = CadenceTexel(clamp(cy + oy, 0.0f, H - 1.0f), P.height);
        const float qkx = clamp(qx + ox, 0.0f, W - 1.0f);
        const float qky = clamp(qy + oy, 0.0f, H - 1.0f);
        float wk = 1.0f - smoothstep(P.depthTol, 2.0f * P.depthTol,
                                     CadenceDepthMismatchAround(CR_DEPTH_NOW(pkx, pky), qkx, qky, P));
        wk *= CadenceColourGate(CR_PROXY_NOW(pkx, pky),
                                CR_PROXY_THEN(CadenceTexel(qkx, P.width), CadenceTexel(qky, P.height)), P.colourTol);
        w += wk * tapW[k];
        wTotal += tapW[k];
        if (k == 0)
            wCentre = wk;
    }
    w = w / wTotal; // exactly 1 when every tap passes
    w *= saturate(2.0f * wCentre);
    w *= wEdge;

    const float qcx = clamp(qx, 0.5f, W - 0.5f);
    const float qcy = clamp(qy, 0.5f, H - 0.5f);

    // Nothing accepted: a caller that multiplies the carry by w gets zero whatever the edit and the fill would have
    // been, so neither the 16-tap edit nor the fill's four rings of eight are read.
    if (stopWhenRejected && w <= 0.0f)
    {
        CadenceCarry none;
        none.add = float3(0.0f, 0.0f, 0.0f);
        none.accept = 0.0f;
        none.qx = qcx;
        none.qy = qcy;
        return none;
    }
    float3 edit = CadenceResidualCatmullRom(qcx, qcy, P);

    // The fill: the model's local tone where the vectors point, scaled by the brightness now over then -- the
    // correction scales with what it corrected (BeliyG3: teeth uncovered by a lip came out 10 % too pale).
    const float lumaNow = max(CadenceLuma(c), 1e-3f);
    const float lumaThen =
        max(CadenceLuma(CR_PROXY_THEN(CadenceTexel(qcx, P.width), CadenceTexel(qcy, P.height))), 1e-3f);
    float3 fill = CadenceLowAt(qcx, qcy, P) * clamp(lumaNow / lumaThen, 0.25f, 4.0f);

    if (w < 0.5f)
    {
        // A disocclusion: at q the last model frame showed the occluder. Look around q for points whose depth
        // then matches this pixel's depth now -- the same surface -- in four rings of eight, turned per pixel
        // so the occluder's outline becomes fine noise rather than a sawtooth; weighted by the depth match and
        // a looser colour gate.
        float3 sum = float3(0.0f, 0.0f, 0.0f);
        float n = 0.0f;
        float matched = 0.0f;
        const float turn = 0.785398f * frac(52.9829189f * frac(0.06711056f * cx + 0.00583715f * cy));
        CR_UNROLL for (int ring = 1; ring <= 4; ++ring)
        {
            const float radius = 12.0f * (float) (1u << ring) * P.scale;
            CR_UNROLL for (int t = 0; t < 8; ++t)
            {
                const float angle = (float) t * 0.785398f + ((ring & 1) == 0 ? 0.392699f : 0.0f) + turn;
                const float rx = clamp(qcx + cos(angle) * radius, 0.5f, W - 0.5f);
                const float ry = clamp(qcy + sin(angle) * radius, 0.5f, H - 0.5f);
                const int tx = CadenceTexel(rx, P.width);
                const int ty = CadenceTexel(ry, P.height);
                float wk =
                    1.0f - smoothstep(P.depthTol, 2.0f * P.depthTol, CadenceDepthMismatch(dc, CR_DEPTH_THEN(tx, ty)));
                const float3 cfk = CR_PROXY_THEN(tx, ty);
                const float gate = CadenceColourGate(c, cfk, 2.0f * P.colourTol);
                wk *= gate;
                matched += wk;
                const float ratio = clamp(lumaNow / max(CadenceLuma(cfk), 1e-3f), 0.25f, 4.0f);
                // Where the colour matches, that spot's own edit rather than the block's mean.
                const float3 rk = CadenceLowAt(rx, ry, P) * (1.0f - gate) + CadenceResidualBilinear(rx, ry, P) * gate;
                sum = sum + rk * (ratio * wk);
                n += wk;
            }
        }
        fill = n > 1e-3f ? sum / n : float3(0.0f, 0.0f, 0.0f);

        // No match, no fill: nothing around shows this surface, so whatever the rings found is the occluder's
        // own edit, and on the floor behind a dark jacket it drew the jacket as a pale silhouette. The pixel
        // shows the game's frame until the model looks again.
        fill = fill * saturate(matched / 0.15f);
    }

    // The fill carries the colour of whatever stood around q; keep its luma and apply it along this pixel's
    // own chroma.
    const float lf = CadenceLuma(fill);
    fill = c * (lf / lumaNow);

    // A NaN in the edit or the fill would survive a zero weight: never let it reach the frame.
    if (!CadenceFinite3(edit))
        edit = float3(0.0f, 0.0f, 0.0f);
    if (!CadenceFinite3(fill))
        fill = float3(0.0f, 0.0f, 0.0f);

    float3 add = fill * (1.0f - w) + edit * w;

    // UI protection: a pixel taken for interface keeps the frame as it is, as the model leaves it on a model
    // frame, rather than receiving scenery's carried edit.
    if (P.haveUi != 0)
        add = add * (1.0f - saturate(CR_UI(px, py)));

    CadenceCarry carry;
    carry.add = add;
    carry.accept = w;
    carry.qx = qcx;
    carry.qy = qcy;
    return carry;
}
