#include "loader/boot/boot.h"
#include "log.h"
#include <string.h>

error_t
pound_boot_init(pound_boot_state_t *state)
{
    if (state == NULL)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }
    memset(state, 0, sizeof(pound_boot_state_t));
    state->initialized = true;
    POUND_LOG_INFO(&thread_logger, "Boot subsystem initialized");
    return POUND_SUCCESS;
}

error_t
pound_boot_load_title(pound_boot_state_t *state, const pound_title_info_t *info)
{
    if (state == NULL || info == NULL)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }
    if (!state->initialized)
    {
        return POUND_ERROR_NOT_INITIALIZED;
    }

    POUND_LOG_INFO(&thread_logger, "Boot: loading title %s (type=%d)", info->path, (int)info->type);
    if (info->is_switch2_compatible)
    {
        state->switch2_mode = true;
        POUND_LOG_INFO(&thread_logger, "Boot: Switch 2 compatibility mode enabled");
    }

    state->booting = true;
    return POUND_SUCCESS;
}

error_t
pound_boot_start(pound_boot_state_t *state)
{
    if (state == NULL)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }
    if (!state->booting)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    POUND_LOG_INFO(&thread_logger, "Boot: starting guest execution");
    return POUND_SUCCESS;
}

bool
pound_boot_is_running(const pound_boot_state_t *state)
{
    if (state == NULL)
    {
        return false;
    }
    return state->booting;
}
