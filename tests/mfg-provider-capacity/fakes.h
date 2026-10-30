#include <sl_dlss_g.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstring>
#include <format>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <hooks/Streamline_DlssgRequest.h>
#define SI_NO_CONVERSION
#include <SimpleIni.h>
#define LOG_INFO(...) ((void) 0)
#define LOG_TRACE(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_FUNC() ((void) 0)
// @optional@
// @serializer@
struct Config
{
    CustomOptional<int, NoDefault> FGDLSSGOverrideInterpolationCount;
    CustomOptional<bool> FGDLSSGOverrideForceDMFG { false };
    CustomOptional<float> FGDLSSGFramerateTargetDMFG { 0.0f };
    CSimpleIniA ini;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
    std::optional<int> readInt(std::string section, std::string key);
    std::optional<std::string> readString(std::string section, std::string key)
    {
        const char* value = ini.GetValue(section.c_str(), key.c_str());
        return value && std::string(value) != "auto" ? std::optional<std::string>(value) : std::nullopt;
    }
    void Read()
    { // @read@
    }
    void Save()
    { // @save@
    }
};
struct feature_version
{
    int major, minor, patch;
    auto operator<=>(const feature_version&) const = default;
};
enum class FGInput
{
    NoFG,
    DLSSG
};
enum class FGOutput
{
    NoFG,
    DLSSG
};
struct State
{
    std::optional<int> dlssgMfgMax;
    feature_version streamlineVersion { 2, 7, 1 };
    FGInput activeFgInput = FGInput::NoFG;
    FGOutput activeFgOutput = FGOutput::NoFG;
    bool dlssgGameDMFGSupported = false;
    std::atomic<bool> dlssgGameRequestsFg { false };
    std::atomic<uint64_t> dlssgGameViewports { 0 };
    sl::DLSSGMode dlssgLastSetMode = sl::DLSSGMode::eOff;
    void* currentFG = nullptr;
    static State& Instance()
    {
        static State s;
        return s;
    }
};
struct Provider
{
    static inline unsigned max = 1, gets = 0;
    static inline sl::Result result = sl::Result::eOk;
    static inline sl::DLSSGOptions sent {};
    static inline void (*duringGet)() = nullptr;
    static sl::Result Get(const sl::ViewportHandle&, sl::DLSSGState& out, const sl::DLSSGOptions*)
    {
        ++gets;
        if (duringGet)
        {
            auto event = duringGet;
            duringGet = nullptr;
            event();
        }
        out.numFramesToGenerateMax = max;
        out.bIsDynamicMFGSupported = sl::Boolean::eTrue;
        return result;
    }
    static sl::Result NewGet(const sl::ViewportHandle& v, sl::DLSSGState& s, const sl::DLSSGOptions* o)
    {
        return Get(v, s, o);
    }
    static inline sl::Result setResult = sl::Result::eOk;
    static sl::Result Set(const sl::ViewportHandle&, const sl::DLSSGOptions& options)
    {
        sent = options;
        return setResult;
    }
};
struct MfgUnlock
{
    static inline uint64_t generation = 0, tries = 0;
    static inline bool patchOnTry = false;
    static uint64_t ProviderGeneration() { return generation; }
    static void TryApply()
    {
        ++tries;
        if (patchOnTry)
        {
            ++generation;
            patchOnTry = false;
        }
    }
};
static std::atomic<unsigned> g_dlssgOptionsGeneration { 0 };
static sl::ViewportHandle lastDlssgViewport { 0 };
static sl::DLSSGOptions lastDlssgOptions {};
void FillDlssgInputState(sl::DLSSGState&) {}
struct StreamlineHooks
{
  private:
    inline static PFun_slDLSSGGetState* o_slDLSSGGetState = Provider::Get;
    inline static PFun_slDLSSGSetOptions* o_slDLSSGSetOptions = Provider::Set;

  public:
    static void RebindGetter(PFun_slDLSSGGetState* getter) { o_slDLSSGGetState = getter; }
    static void applyMenuDlssgInterlock(sl::DLSSGOptions&, bool) {}
    static inline thread_local bool replayingDlssgOptions = false;
    static void updateDlssgOptions();
    static sl::Result hkslDLSSGSetOptions(const sl::ViewportHandle&, const sl::DLSSGOptions&);
    static sl::Result hkslDLSSGGetState(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);
};
