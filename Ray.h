#pragma once

#include "Config.h"
#include "Log.h"
#include "RendererBase.h"

/**
  @file Ray.h
*/

namespace Ray {
extern LogNull g_null_log;
extern LogStdout g_stdout_log;

/// Default renderer flags used to choose backend, by default tries to create gpu renderer first
const Bitmask<eRendererType> DefaultEnabledRenderTypes =
    Bitmask<eRendererType>{eRendererType::Reference} | eRendererType::SIMD_SSE2 | eRendererType::SIMD_AVX |
    eRendererType::SIMD_AVX2 | eRendererType::SIMD_NEON | eRendererType::Vulkan | eRendererType::DirectX12;

/** @brief Creates renderer
    @return pointer to created renderer
*/
RendererBase *CreateRenderer(const settings_t &s, ILog *log = &g_null_log,
                             Bitmask<eRendererType> enabled_types = DefaultEnabledRenderTypes);

int QueryAvailableGPUDevices(ILog *log, gpu_device_t out_devices[], int capacity);

bool MatchDeviceNames(const char *name, const char *pattern);
} // namespace Ray