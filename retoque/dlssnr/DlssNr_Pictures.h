#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>

// Which of the game's DLSS features NR follows (docs/dlssnr/design/main-picture.md, tested by tests/nr-main-picture).
//
// A game can keep more than one DLSS feature alive and evaluate them in turn: Neverness to Everness evaluates a second
// one for the 3D character preview of some menu pages, next to the world's (reported in janblade/OptiScaler-F5-DLSSNR-
// Multipass #285; no code taken). NR has one model, one history and one set of settling gates, so the two pictures
// took turns in them. At different sizes or formats the gates restarted on every call and the world lost NR for as
// long as the preview was shown; at the same size each picture was reconstructed over the other's history.
//
// NR follows one feature, the main picture. Any other feature is left as it is: the evaluate passes untouched and no
// NR state sees it. A feature whose colour is main's allocation is a view of main's picture (a multi-monitor layout,
// one feature per screen) and goes on as before, to the multi-view rule. A game with one feature is never judged
// otherwise than main.
//
// Which feature is main: the first one. A picture clearly larger than main's takes it (the world, not a preview).
// After the game releases main, a feature not yet known as another picture takes it at once (the same picture made
// again); a known other picture waits kQuietCalls calls for that successor. Main that stops calling while others call
// for kQuietCalls calls is quiet (a menu scene's feature the game never released), and the next caller takes it.
//
// Each change of the picture NR follows -- a takeover by size or by quiet, or a known other picture taking main after
// waiting -- opens a new picture epoch. The epoch travels with the evaluate's frame to whoever holds the model's
// history (EpochGate, under that route's own lock): a newer epoch starts the history and the exposure meter over, an
// older one is a frame of a picture already replaced and is refused. Taking main first or as an unknown successor
// keeps the epoch, so a game with one feature, made again, keeps exactly what it had.
//
// Counted in calls, not presents: a present is not observed where the swapchain is not wrapped. Standard library only;
// the glue (DlssNr_PictureGate.h) reads the parameters and logs.
namespace DlssNr::Pictures
{
enum class Role : uint8_t
{
    Main,       // the picture NR follows
    ViewOfMain, // another feature on main's colour allocation: goes on as main's picture does
    Aside,      // another picture: left as it is
};

// How a call came to be main, when it did on this call.
enum class Took : uint8_t
{
    No,     // it did not take main on this call
    First,  // nothing was main yet
    Vacant, // main was released
    Larger, // its picture is clearly larger than main's
    Quiet,  // main stopped calling while others called
    Waited, // main was released, and this known other picture took it after waiting for a successor
};

// Whether taking main this way shows the model another picture: a new picture epoch.
constexpr bool NewPicture(Took took) { return took == Took::Larger || took == Took::Quiet || took == Took::Waited; }

struct Call
{
    uint64_t key = 0;     // the input API and the game's NGX handle (RenderGeometry::Key)
    uint64_t area = 0;    // output width times height, 0 when unknown
    uintptr_t colour = 0; // identity of the colour allocation, 0 when unknown
};

struct Judgement
{
    Role role = Role::Main;
    Took took = Took::No;
    uint64_t previous = 0;   // the key that was main before this call took it (Larger, Quiet), else 0
    bool firstAside = false; // the first time this feature is left aside since it became known
    uint64_t epoch = 0;      // the picture epoch of a Main or ViewOfMain call (from 1); 0 for Aside
};

// Judged evaluates by other features without one from main before main counts as quiet, and how long a known other
// picture waits for main's successor after main's release. Both seams of one evaluate count once: with one other
// feature evaluated per frame, 16 frames.
inline constexpr uint32_t kQuietCalls = 16;

// Clearly larger: a quarter more output area. Dynamic resolution moves the render size, not the output.
constexpr bool ClearlyLarger(uint64_t area, uint64_t mainArea) { return mainArea != 0 && area * 4 > mainArea * 5; }

class Rule
{
    uint64_t _main = 0;
    uint64_t _mainArea = 0;
    uintptr_t _mainColour = 0;
    uint32_t _quiet = 0;                  // calls by other pictures since main last called
    uint32_t _vacantFor = 0;              // calls since main was released, while it is vacant
    bool _released = false;               // main was released and nothing has taken it since
    uint64_t _epoch = 1;                  // the picture epoch: advances on each NewPicture takeover
    std::unordered_set<uint64_t> _others; // features known as other pictures, until they are released or take main

    Judgement Take(const Call& call, Took took)
    {
        if (NewPicture(took))
            ++_epoch;
        Judgement judgement { Role::Main, took, (took == Took::Larger || took == Took::Quiet) ? _main : 0, false,
                              _epoch };
        if (judgement.previous != 0)
            _others.insert(judgement.previous);
        _others.erase(call.key);
        _main = call.key;
        _mainArea = call.area;
        _mainColour = call.colour;
        _quiet = 0;
        _vacantFor = 0;
        _released = false;
        return judgement;
    }

    Judgement Aside(const Call& call)
    {
        Judgement judgement { Role::Aside, Took::No, 0, false, 0 };
        judgement.firstAside = _others.insert(call.key).second;
        return judgement;
    }

  public:
    Judgement Judge(const Call& call)
    {
        if (_main == 0)
        {
            if (!_released)
                return Take(call, Took::First);
            // A known other picture (the preview, still open) waits for main's successor; anything else is it.
            if (_others.count(call.key) == 0)
                return Take(call, Took::Vacant);
            if (++_vacantFor >= kQuietCalls)
                return Take(call, Took::Waited);
            return Aside(call);
        }

        if (call.key == _main)
        {
            _mainArea = call.area;
            _mainColour = call.colour;
            _quiet = 0;
            return { Role::Main, Took::No, 0, false, _epoch };
        }

        if (call.colour != 0 && call.colour == _mainColour)
        {
            _quiet = 0;
            return { Role::ViewOfMain, Took::No, 0, false, _epoch };
        }

        if (ClearlyLarger(call.area, _mainArea))
            return Take(call, Took::Larger);

        if (++_quiet >= kQuietCalls)
            return Take(call, Took::Quiet);

        return Aside(call);
    }

    // The game released a feature. Returns whether it was main.
    bool Release(uint64_t key)
    {
        _others.erase(key);
        if (key == 0 || key != _main)
            return false;
        _main = 0;
        _mainArea = 0;
        _mainColour = 0;
        _quiet = 0;
        _vacantFor = 0;
        _released = true;
        return true;
    }

    uint64_t Main() const { return _main; }
    size_t Others() const { return _others.size(); }
    uint64_t Epoch() const { return _epoch; }
};

// What the holder of the model's history does with a frame's picture epoch.
enum class Consumed : uint8_t
{
    Same,    // the picture it has been showing the model, or no epoch at all (an evaluate the rule never judged)
    Restart, // a newer picture: start the history and the exposure meter over
    Stale,   // a frame of a picture already replaced: refuse it
};

// One per holder of a model's history, used only under that holder's lock. The first epoch it sees is its picture,
// whatever its number: nothing came before it to start over from.
class EpochGate
{
    uint64_t _seen = 0;

  public:
    Consumed Consume(uint64_t epoch)
    {
        if (epoch == 0 || epoch == _seen)
            return Consumed::Same;
        if (_seen == 0)
        {
            _seen = epoch;
            return Consumed::Same;
        }
        if (epoch < _seen)
            return Consumed::Stale;
        _seen = epoch;
        return Consumed::Restart;
    }
};

// The process's rule. Evaluates arrive on the game's render threads and releases on any thread.
class Registry
{
    std::mutex _mutex;
    Rule _rule;

  public:
    Judgement Judge(const Call& call)
    {
        std::lock_guard lock(_mutex);
        return _rule.Judge(call);
    }
    bool Release(uint64_t key)
    {
        std::lock_guard lock(_mutex);
        return _rule.Release(key);
    }
    uint64_t Main()
    {
        std::lock_guard lock(_mutex);
        return _rule.Main();
    }
};

inline Registry& Instance()
{
    static Registry registry;
    return registry;
}
} // namespace DlssNr::Pictures
