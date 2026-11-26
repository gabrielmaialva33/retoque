#pragma once
#include <cstdint>
enum NvAPI_Status
{
    NVAPI_ERROR = -1,
    NVAPI_SETTING_NOT_FOUND = -160,
    NVAPI_OK = 0
};
enum NVDRS_SETTING_TYPE
{
    NVDRS_DWORD_TYPE = 0,
    NVDRS_BINARY_TYPE = 1
};
enum NVDRS_SETTING_LOCATION
{
    NVDRS_CURRENT_PROFILE_LOCATION = 0,
    NVDRS_GLOBAL_PROFILE_LOCATION = 1
};
constexpr uint32_t NVDRS_SETTING_VER = 7;
struct NVDRS_SETTING
{
    uint32_t version;
    uint16_t settingName[64];
    uint32_t settingId;
    NVDRS_SETTING_TYPE settingType;
    NVDRS_SETTING_LOCATION settingLocation;
    uint32_t isCurrentPredefined, isPredefinedValid, u32PredefinedValue, u32CurrentValue;
};
struct NvApiHooks
{
    inline static void* o_NvAPI_DRS_GetSetting = nullptr;
};
