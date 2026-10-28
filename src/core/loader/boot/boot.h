#ifndef POUND_LOADER_BOOT_BOOT_H
#define POUND_LOADER_BOOT_BOOT_H

#include "attributes.h"
#include "errors.h"
#include "loader/title_loader.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool initialized;
    bool booting;
    bool switch2_mode;
} pound_boot_state_t;

error_t pound_boot_init(pound_boot_state_t *state);
error_t pound_boot_load_title(pound_boot_state_t *state, const pound_title_info_t *info);
error_t pound_boot_start(pound_boot_state_t *state);
bool pound_boot_is_running(const pound_boot_state_t *state);

#ifdef __cplusplus
}
#endif

#endif // POUND_LOADER_BOOT_BOOT_H
