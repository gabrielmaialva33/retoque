
// The NR settings as NVIDIA's R610+ NvApiDriverSettings.h numbers them (NVIDIA/nvapi 87dca625, the headers
// dxvk-nvapi v0.9.3 builds with). Spelled out here, not taken from production, so a wrong constant there fails.
constexpr NvU32 kNrOverride = 0x10E41E04;        // NGX_DLSS_NR_OVERRIDE_ID: 0 off, 1 on
constexpr NvU32 kNrPresetSelection = 0x10E41DF8; // NGX_DLSS_NR_OVERRIDE_RENDER_PRESET_SELECTION_ID
constexpr NvU32 kNrSlOverride = 0x10E41E05;      // NGX_DLSS_NR_SL_OVERRIDE_ID: 0 off, 1 on
constexpr NvU32 kSlDlssOverride = 0x10E41E06;    // SL_DLSS_OVERRIDE_ID (the hook's older 0x10e41e06)
constexpr NvU32 kPresetA = 1, kPresetB = 2, kPresetD = 4, kPresetLatest = 0x00FFFFFF;
constexpr NvU32 kUnrelated = 0x12345678;

// One DRS value the fake driver answers with.
struct DriverAnswer
{
    NvAPI_Status status = NVAPI_OK;
    NVDRS_SETTING_TYPE type = NVDRS_DWORD_TYPE;
    NvU32 value = 0;
};
inline DriverAnswer driver;
inline unsigned driverCalls = 0;

NvAPI_Status FakeGetSetting(NvDRSSessionHandle, NvDRSProfileHandle, NvU32 settingId, NVDRS_SETTING* setting)
{
    ++driverCalls;
    if (setting == nullptr || driver.status != NVAPI_OK)
        return driver.status;

    setting->settingId = settingId;
    setting->settingType = driver.type;
    if (driver.type == NVDRS_DWORD_TYPE)
        setting->u32CurrentValue = driver.value;
    else
        for (unsigned i = 0; i < 8; ++i)
            setting->wszCurrentValue[i] = static_cast<unsigned short>(0x41 + i);
    return NVAPI_OK;
}

struct Read
{
    NvAPI_Status status;
    NVDRS_SETTING setting;
    NVDRS_SETTING asDriverWrote;
};

// What a reader of `settingId` sees through the hook, next to what the driver itself wrote.
Read ReadThroughHook(NvU32 settingId, DriverAnswer answer)
{
    driver = answer;
    Read read {};
    std::memset(&read.setting, 0xCD, sizeof(read.setting));
    read.asDriverWrote = read.setting;
    FakeGetSetting(nullptr, nullptr, settingId, &read.asDriverWrote);
    read.status = NvApiHooks::hkNvAPI_DRS_GetSetting(nullptr, nullptr, settingId, &read.setting);
    return read;
}

// Log lines that name this setting id among their values.
unsigned LinesNaming(NvU32 settingId)
{
    unsigned count = 0;
    for (const auto& record : logRecords)
        if (std::find(record.values.begin(), record.values.end(), std::to_string(settingId)) != record.values.end())
            ++count;
    return count;
}

bool LastLineNamesValue(NvU32 settingId, NvU32 value)
{
    for (auto it = logRecords.rbegin(); it != logRecords.rend(); ++it)
    {
        const auto& values = it->values;
        if (std::find(values.begin(), values.end(), std::to_string(settingId)) == values.end())
            continue;
        return it->level == "info" && std::find(values.begin(), values.end(), std::to_string(value)) != values.end();
    }
    return false;
}

void ExpectUntouched(const Read& read)
{
    assert(read.status == NVAPI_OK);
    assert(std::memcmp(&read.setting, &read.asDriverWrote, sizeof(read.setting)) == 0);
}

// NR off (the default): the NR settings reach the reader exactly as the driver wrote them, and nothing is logged.
void NrOffPassesEverythingThrough()
{
    Config::Instance()->DlssNrEnabled.value = false;
    for (NvU32 id : { kNrOverride, kNrPresetSelection, kNrSlOverride })
        for (NvU32 value : { NvU32 { 0 }, NvU32 { 1 }, kPresetD, kPresetLatest })
            ExpectUntouched(ReadThroughHook(id, { NVAPI_OK, NVDRS_DWORD_TYPE, value }));

    for (NvU32 id : { kNrOverride, kNrPresetSelection, kNrSlOverride })
        assert(LinesNaming(id) == 0);
}

// The SR/RR preset neutralisation is the same with NR off and on.
void SrRrUnchanged(bool nrEnabled)
{
    Config::Instance()->DlssNrEnabled.value = nrEnabled;
    auto& state = State::Instance();

    auto sr = ReadThroughHook(NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_ID, { NVAPI_OK, NVDRS_DWORD_TYPE, 11 });
    assert(sr.status == NVAPI_OK && sr.setting.u32CurrentValue == NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_OFF);
    assert(state.dlssRenderPresetExternal == 11 && state.dlssPresetsOverriddenExternally);

    auto rr = ReadThroughHook(NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_ID, { NVAPI_OK, NVDRS_DWORD_TYPE, 4 });
    assert(rr.status == NVAPI_OK && rr.setting.u32CurrentValue == NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_OFF);
    assert(state.dlssdRenderPresetExternal == 4 && state.dlssdPresetsOverriddenExternally);

    sr = ReadThroughHook(NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_ID, { NVAPI_OK, NVDRS_DWORD_TYPE, 0 });
    assert(sr.setting.u32CurrentValue == 0 && !state.dlssPresetsOverriddenExternally);

    // Not an NR setting, and the Streamline override with no DLSSG output: as the driver wrote them.
    ExpectUntouched(ReadThroughHook(kUnrelated, { NVAPI_OK, NVDRS_DWORD_TYPE, 7 }));
    ExpectUntouched(ReadThroughHook(kSlDlssOverride, { NVAPI_OK, NVDRS_DWORD_TYPE, 1 }));
    assert(LinesNaming(kUnrelated) == 0 && LinesNaming(kSlDlssOverride) == 0);
}

// NR on: the preset selection is recorded, logged when it changes, and reported as off to whoever reads it.
void NrOnNeutralisesThePresetSelection()
{
    Config::Instance()->DlssNrEnabled.value = true;
    const unsigned before = LinesNaming(kNrPresetSelection);

    auto read = ReadThroughHook(kNrPresetSelection, { NVAPI_OK, NVDRS_DWORD_TYPE, kPresetA });
    assert(read.status == NVAPI_OK);
    assert(read.setting.u32CurrentValue == 0);
    assert(read.setting.settingId == kNrPresetSelection && read.setting.settingType == NVDRS_DWORD_TYPE);
    assert(LinesNaming(kNrPresetSelection) == before + 1 && LastLineNamesValue(kNrPresetSelection, kPresetA));

    // The same value again: neutralised again, logged once.
    read = ReadThroughHook(kNrPresetSelection, { NVAPI_OK, NVDRS_DWORD_TYPE, kPresetA });
    assert(read.setting.u32CurrentValue == 0);
    assert(LinesNaming(kNrPresetSelection) == before + 1);

    // A different value is a new observation.
    read = ReadThroughHook(kNrPresetSelection, { NVAPI_OK, NVDRS_DWORD_TYPE, kPresetLatest });
    assert(read.setting.u32CurrentValue == 0);
    assert(LinesNaming(kNrPresetSelection) == before + 2 && LastLineNamesValue(kNrPresetSelection, kPresetLatest));

    // Off is already what the reader should see; it is still said once, so the log shows the driver was asked.
    read = ReadThroughHook(kNrPresetSelection, { NVAPI_OK, NVDRS_DWORD_TYPE, 0 });
    assert(read.setting.u32CurrentValue == 0);
    assert(LinesNaming(kNrPresetSelection) == before + 3 && LastLineNamesValue(kNrPresetSelection, 0));
    read = ReadThroughHook(kNrPresetSelection, { NVAPI_OK, NVDRS_DWORD_TYPE, 0 });
    assert(LinesNaming(kNrPresetSelection) == before + 3);

    // The SR/RR state is not where the NR value goes.
    assert(State::Instance().dlssRenderPresetExternal != kPresetLatest);
    assert(State::Instance().dlssdRenderPresetExternal != kPresetLatest);
}

// NR on: the two enable switches are reported (logged once per value), not changed.
void NrOnReportsTheEnableSwitches()
{
    Config::Instance()->DlssNrEnabled.value = true;
    for (NvU32 id : { kNrOverride, kNrSlOverride })
    {
        const unsigned before = LinesNaming(id);
        ExpectUntouched(ReadThroughHook(id, { NVAPI_OK, NVDRS_DWORD_TYPE, 1 }));
        assert(LinesNaming(id) == before + 1 && LastLineNamesValue(id, 1));
        ExpectUntouched(ReadThroughHook(id, { NVAPI_OK, NVDRS_DWORD_TYPE, 1 }));
        assert(LinesNaming(id) == before + 1);
    }
}

// NR on, but nothing a DWORD write could safely act on: a failed read, a non-DWORD value, no output.
void NrOnLeavesWhatItCannotReadAlone()
{
    Config::Instance()->DlssNrEnabled.value = true;
    const unsigned before = LinesNaming(kNrPresetSelection);

    auto read = ReadThroughHook(kNrPresetSelection, { NVAPI_SETTING_NOT_FOUND, NVDRS_DWORD_TYPE, kPresetB });
    assert(read.status == NVAPI_SETTING_NOT_FOUND);
    assert(std::memcmp(&read.setting, &read.asDriverWrote, sizeof(read.setting)) == 0);

    ExpectUntouched(ReadThroughHook(kNrPresetSelection, { NVAPI_OK, NVDRS_WSTRING_TYPE, 0 }));

    driver = { NVAPI_OK, NVDRS_DWORD_TYPE, kPresetB };
    assert(NvApiHooks::hkNvAPI_DRS_GetSetting(nullptr, nullptr, kNrPresetSelection, nullptr) == NVAPI_OK);

    assert(LinesNaming(kNrPresetSelection) == before);
}

// NR switched off at runtime: back to passing the value through, with no further lines.
void NrOffAgainPassesThrough()
{
    Config::Instance()->DlssNrEnabled.value = false;
    const unsigned before = logRecords.size();
    ExpectUntouched(ReadThroughHook(kNrPresetSelection, { NVAPI_OK, NVDRS_DWORD_TYPE, kPresetB }));
    ExpectUntouched(ReadThroughHook(kNrOverride, { NVAPI_OK, NVDRS_DWORD_TYPE, 0 }));
    assert(logRecords.size() == before);
}

void NoDriver()
{
    NvApiHooks::o_NvAPI_DRS_GetSetting = nullptr;
    const unsigned calls = driverCalls;
    NVDRS_SETTING setting {};
    assert(NvApiHooks::hkNvAPI_DRS_GetSetting(nullptr, nullptr, kNrPresetSelection, &setting) == NVAPI_ERROR);
    assert(driverCalls == calls);
}

int main()
{
    NvApiHooks::o_NvAPI_DRS_GetSetting = &FakeGetSetting;

    NrOffPassesEverythingThrough();
    SrRrUnchanged(false);
    NrOnNeutralisesThePresetSelection();
    NrOnReportsTheEnableSwitches();
    NrOnLeavesWhatItCannotReadAlone();
    SrRrUnchanged(true);
    NrOffAgainPassesThrough();
    NoDriver();

    std::puts("nr-drs-override: NR off passes NR DRS settings through untouched; NR on neutralises the NR "
              "preset selection, reports both enable switches, logs each value once; SR/RR unchanged");
    return 0;
}
