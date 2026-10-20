// The replay recorder's state machine and manifest against a scripted backend.
#include <dlssnr/DlssNr_ReplayRecorder.h>

#include <cstdio>
#include <map>
#include <string>

using namespace DlssNr::ReplayRecorder;

namespace
{
int failures = 0;
int checks = 0;
void Expect(bool ok, const char* what)
{
    ++checks;
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct Fake
{
    std::map<size_t, Certificate> answer; // per slot; Pending when absent
    std::vector<uint32_t> handed;         // frame ids, in order
    std::vector<FrameMeta> metas;         // what was handed, in order
    std::vector<size_t> released;
    int copies = 0;
    bool copyFails = false;
    int finished = -1;
    std::string stopped;
    std::map<size_t, uint32_t> slotFrame;

    bool Copy(size_t slot, int&)
    {
        if (copyFails)
            return false;
        ++copies;
        answer.erase(slot);
        return true;
    }
    Certificate Certify(size_t slot) { return answer.count(slot) ? answer[slot] : Certificate::Pending; }
    void Hand(size_t slot, const FrameMeta& meta)
    {
        handed.push_back(meta.id);
        metas.push_back(meta);
        answer.erase(slot);
    }
    void Release(size_t slot)
    {
        released.push_back(slot);
        answer.erase(slot);
    }
    void Finish(uint32_t frames, const char* why)
    {
        finished = static_cast<int>(frames);
        stopped = why ? why : "";
    }
    void AcceptAll()
    {
        for (size_t i = 0; i < kSlots; ++i)
            answer[i] = Certificate::Accepted;
    }
};

FrameMeta Meta(uint64_t present)
{
    FrameMeta m;
    m.present = present;
    m.color = { 10, 64, 32, 8 };
    m.depth = { 41, 32, 16, 4 };
    m.motion = { 34, 32, 16, 4 };
    m.frameTimeMs = 16.0f;
    return m;
}
} // namespace

int main()
{
    // Idle: nothing happens.
    {
        Session<Fake> s;
        Fake b;
        int rec = 0;
        s.AtBoundary(b, rec, Meta(1), 0.0, nullptr);
        s.Poll(b);
        Expect(s.Current() == Phase::Idle && b.copies == 0 && b.finished == -1, "idle records nothing");
    }
    // A refusal on the first frame fails the request with its reason; nothing is copied.
    {
        Session<Fake> s;
        Fake b;
        int rec = 0;
        Expect(s.Request(), "request accepted");
        s.AtBoundary(b, rec, Meta(1), 0.0, "no route");
        Expect(s.Current() == Phase::Failed && std::string(s.Reason()) == "no route" && b.copies == 0,
               "first-frame refusal fails with its reason");
        Expect(s.Request(), "a failed session can be asked again");
    }
    // A normal clip: frames until the length; certified in order; finished with the contiguous count.
    {
        Session<Fake> s;
        Fake b;
        int rec = 0;
        s.Request();
        Expect(!s.Request(), "a second request while one is pending is refused");
        double t = 1000.0;
        uint64_t present = 10;
        while (s.Current() == Phase::Requested || s.Current() == Phase::Recording)
        {
            s.Poll(b);
            s.AtBoundary(b, rec, Meta(present), t, nullptr);
            b.AcceptAll(); // the GPU and Reset keep up
            t += 50.0;     // 20 fps
            present += 4;  // MFG 4x presents per evaluation
        }
        s.Poll(b);
        Expect(s.Current() == Phase::Done, "a full clip ends Done");
        Expect(b.finished == 120 && s.Recorded() == 120 && s.Handed() == 120, "6 s at 20 fps is 120 frames");
        bool ordered = true;
        for (size_t i = 0; i < b.handed.size(); ++i)
            ordered = ordered && b.handed[i] == i + 1;
        Expect(ordered, "frames handed in id order, consecutive");
        Expect(b.stopped == "the clip reached its length", "stops for its length");
    }
    // The frame cap bounds a fast game.
    {
        Session<Fake> s;
        Fake b;
        int rec = 0;
        s.Request();
        double t = 0.0;
        for (int i = 0; i < 1000 && s.Current() != Phase::Done; ++i, t += 5.0)
        {
            s.Poll(b);
            s.AtBoundary(b, rec, Meta(i + 1), t, nullptr);
            b.AcceptAll();
        }
        s.Poll(b);
        Expect(b.finished == int(kMaxFrames), "the frame cap holds at 200 fps");
    }
    // Backpressure: nothing certifies; the seventh frame ends the clip; the six in flight are handed later.
    {
        Session<Fake> s;
        Fake b;
        int rec = 0;
        s.Request();
        for (int i = 0; i < 10; ++i)
        {
            s.Poll(b);
            s.AtBoundary(b, rec, Meta(i + 1), i * 16.0, nullptr);
        }
        Expect(s.Current() == Phase::Finishing && s.Recorded() == kSlots,
               "a full ring ends the clip, no frame skipped");
        Expect(std::string(s.Reason()).find("readback slot") != std::string::npos, "the reason names the ring");
        b.AcceptAll();
        s.Poll(b);
        Expect(s.Current() == Phase::Done && b.finished == int(kSlots), "the in-flight frames still make the clip");
    }
    // A frame discarded in the middle ends the clip at the frames before it; later ones are released, not read.
    {
        Session<Fake> s;
        Fake b;
        int rec = 0;
        s.Request();
        for (int i = 0; i < 5; ++i)
            s.AtBoundary(b, rec, Meta(i + 1), i * 16.0, nullptr);
        b.answer[0] = b.answer[1] = b.answer[2] = Certificate::Accepted;
        b.answer[3] = Certificate::Discarded;
        b.answer[4] = Certificate::Accepted;
        s.Poll(b);
        Expect(b.handed.size() == 3 && b.finished == 3, "the clip is the certified prefix");
        Expect(b.released.size() == 2, "the discarded frame and the one after it are released unread");
        Expect(s.Current() == Phase::Done, "a prefix clip is still a clip");
    }
    // A failed certificate keeps its slot busy, ends the clip, and later frames are released unread once answered.
    {
        Session<Fake> s;
        Fake b;
        int rec = 0;
        s.Request();
        for (int i = 0; i < 3; ++i)
            s.AtBoundary(b, rec, Meta(i + 1), i * 16.0, nullptr);
        b.answer[0] = Certificate::Accepted;
        b.answer[1] = Certificate::Failed;
        b.answer[2] = Certificate::Accepted;
        s.Poll(b);
        Expect(b.handed.size() == 1 && b.finished == 1, "a failed completion ends the clip after the frames before it");
        Expect(b.released.size() == 1, "the frame after a failure is released unread, the failed slot is not");
    }
    // A copy that cannot be recorded ends the clip; an empty clip fails.
    {
        Session<Fake> s;
        Fake b;
        b.copyFails = true;
        int rec = 0;
        s.Request();
        s.AtBoundary(b, rec, Meta(1), 0.0, nullptr);
        s.Poll(b);
        Expect(s.Current() == Phase::Failed && b.finished == 0, "no recorded frame fails the clip");
    }
    // The first frame is a reset whatever the game said; ids and clocks are the clip's own.
    {
        Session<Fake> s;
        Fake b;
        int rec = 0;
        s.Request();
        for (int i = 0; i < 3; ++i)
            s.AtBoundary(b, rec, Meta(100 + i), 5000.0 + i * 20.0, nullptr);
        b.AcceptAll();
        s.Poll(b);
        Expect(b.metas.size() == 3 && b.metas[0].reset && !b.metas[1].reset && !b.metas[2].reset,
               "the clip's first frame is a reset, the others keep the game's flag");
        Expect(b.metas[0].id == 1 && b.metas[2].id == 3 && b.metas[0].timeMs == 0.0 && b.metas[2].timeMs == 40.0,
               "ids from 1 and time from the clip's first frame");
        Expect(b.metas[1].present == 101, "the present counter is the game's");
    }
    // Refusals.
    {
        RefusalInputs r;
        r.freeBytes = 100;
        r.estimateBytes = 10;
        Expect(RefusalFor(r) == nullptr, "a usable route with space starts");
        r.afterUpscaleD3D12 = false;
        Expect(RefusalFor(r) != nullptr, "wrong route refused");
        r.afterUpscaleD3D12 = true;
        r.exposureTextureBound = true;
        Expect(RefusalFor(r) != nullptr, "exposure texture refused");
        r.exposureTextureBound = false;
        r.freeBytes = 12;
        Expect(RefusalFor(r) != nullptr, "space below 1.25x the estimate refused");
    }
    // The manifest: frames, warmup split, escaping. Printed for run.py to parse.
    {
        std::vector<WrittenFrame> frames;
        for (uint32_t i = 1; i <= 4; ++i)
        {
            WrittenFrame f;
            f.meta = Meta(10 + i);
            f.meta.id = i;
            f.meta.timeMs = (i - 1) * 1000.0;
            f.meta.reset = i == 1;
            f.meta.createFlags = 1u | kFlagMvLowRes;
            f.colorSha = f.depthSha = f.motionSha = std::string(64, 'a');
            frames.push_back(f);
        }
        Provenance p;
        p.exe = "Cyber\"punk";
        p.settings = "a\nb";
        Expect(Finite(frames[0].meta), "finite meta");
        FrameMeta bad = frames[0].meta;
        bad.jitterX = NAN;
        Expect(!Finite(bad), "a NaN is caught before the manifest");
        std::FILE* out = std::fopen("manifest.json", "w");
        Expect(out != nullptr, "manifest written");
        if (out)
        {
            std::fputs(CaptureJson(frames, p, "the clip reached its length").c_str(), out);
            std::fclose(out);
        }
    }

    if (failures)
        return 1;
    std::printf("PASS %d recorder checks: idle, first-frame refusal, length and frame caps, ring backpressure, "
                "certified prefix, discarded and failed completions, copy failure, refusals, manifest\n",
                checks);
    return 0;
}
