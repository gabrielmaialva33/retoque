// NR uses RGBA16F working images. Keep the upstream filter arithmetic and shaders intact.
#if defined(NR_SCALER_BCUS)
#include "dlssnr_scaler_bcus.hlsl"
#else
#define RWTexture2D [[vk::image_format("rgba16f")]] RWTexture2D
#if defined(NR_SCALER_BCDS_BICUBIC)
#include "shaders/output_scaling/precompile/bcds_bicubic.hlsl"
#elif defined(NR_SCALER_BCDS_CATMULL)
#include "shaders/output_scaling/precompile/bcds_catmull.hlsl"
#elif defined(NR_SCALER_BCDS_LANCZOS2)
#include "shaders/output_scaling/precompile/bcds_lanczos2.hlsl"
#elif defined(NR_SCALER_BCDS_LANCZOS3)
#include "shaders/output_scaling/precompile/bcds_lanczos3.hlsl"
#elif defined(NR_SCALER_BCDS_KAISER2)
#include "shaders/output_scaling/precompile/bcds_kaiser2.hlsl"
#elif defined(NR_SCALER_BCDS_KAISER3)
#include "shaders/output_scaling/precompile/bcds_kaiser3.hlsl"
#elif defined(NR_SCALER_BCDS_MAGC)
#include "shaders/output_scaling/precompile/bcds_magc.hlsl"
#elif defined(NR_SCALER_FSR_EASU)
#include "shaders/output_scaling/fsr1/fsr_easu.hlsl"
#else
#error Select exactly one NR_SCALER variant through build_scaler_vk.py
#endif
#undef RWTexture2D
#endif
