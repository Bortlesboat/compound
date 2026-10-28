#ifndef POUND_GPU_VULKAN_BACKEND_H
#define POUND_GPU_VULKAN_BACKEND_H

#include "attributes.h"
#include "errors.h"
#include "gpu/sm86/translate.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pound_vulkan_backend pound_vulkan_backend_t;

typedef struct
{
    const char *application_name;
    uint32_t application_version;
    uint32_t engine_version;
    const char **instance_extensions;
    uint32_t instance_extension_count;
    const char **device_extensions;
    uint32_t device_extension_count;
} pound_vulkan_config_t;

/// Initialize Vulkan backend. This creates instance, device, and queue if possible.
/// Returns POUND_SUCCESS if initialized (may use a fallback stub if Vulkan SDK not available).
error_t pound_vulkan_init(pound_vulkan_backend_t **backend, const pound_vulkan_config_t *config);

/// Shutdown and free Vulkan backend.
void pound_vulkan_shutdown(pound_vulkan_backend_t *backend);

/// Compile/upload SPIR-V shader module. SPIR-V words come from sm86_translate.
error_t pound_vulkan_create_shader(pound_vulkan_backend_t *backend,
                                  const uint32_t *spirv_words,
                                  size_t spirv_word_count,
                                  uint64_t *out_shader_id);

/// Destroy shader module.
error_t pound_vulkan_destroy_shader(pound_vulkan_backend_t *backend, uint64_t shader_id);

/// Submit a draw/compute work with bound shaders and resources. Simplified interface.
error_t pound_vulkan_submit(pound_vulkan_backend_t *backend);

/// Present current frame.
error_t pound_vulkan_present(pound_vulkan_backend_t *backend);

/// Check if backend is properly initialized with real Vulkan.
bool pound_vulkan_is_available(const pound_vulkan_backend_t *backend);

#ifdef __cplusplus
}
#endif

#endif // POUND_GPU_VULKAN_BACKEND_H
