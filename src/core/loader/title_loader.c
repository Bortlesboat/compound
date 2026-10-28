#include "loader/title_loader.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

error_t
pound_title_detect(const char *path, pound_title_info_t *info)
{
    if (path == NULL || info == NULL)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    memset(info, 0, sizeof(pound_title_info_t));
    strncpy(info->path, path, sizeof(info->path) - 1);
    info->path[sizeof(info->path) - 1] = '\0';

    const char *ext = strrchr(path, '.');
    if (ext != NULL)
    {
        if (strcmp(ext, ".nro") == 0)
        {
            info->type = POUND_TITLE_NRO;
            info->is_switch2_compatible = true;
            return POUND_SUCCESS;
        }
        else if (strcmp(ext, ".nso") == 0)
        {
            info->type = POUND_TITLE_NSO;
            info->is_switch2_compatible = true;
            return POUND_SUCCESS;
        }
        else if (strcmp(ext, ".nca") == 0)
        {
            info->type = POUND_TITLE_NCA;
            info->is_switch2_compatible = true;
            return POUND_SUCCESS;
        }
        else if (strcmp(ext, ".nsp") == 0)
        {
            info->type = POUND_TITLE_NSP;
            info->is_switch2_compatible = true;
            return POUND_SUCCESS;
        }
        else if (strcmp(ext, ".xci") == 0)
        {
            info->type = POUND_TITLE_XCI;
            info->is_switch2_compatible = true;
            return POUND_SUCCESS;
        }
        else if (strcmp(ext, ".hac") == 0)
        {
            info->type = POUND_TITLE_HAC;
            info->is_switch2_compatible = true;
            return POUND_SUCCESS;
        }
    }

    info->type = POUND_TITLE_UNKNOWN;
    info->is_switch2_compatible = false;
    POUND_LOG_WARN(&thread_logger, "Unknown title format for %s", path);
    return POUND_SUCCESS;
}

error_t
pound_title_load(const pound_title_info_t *info)
{
    if (info == NULL)
    {
        return POUND_ERROR_INVALID_ARGUMENT;
    }

    POUND_LOG_INFO(&thread_logger, "Loading title: %s (type=%d, switch2_compat=%d)",
                   info->path,
                   (int)info->type,
                   info->is_switch2_compatible ? 1 : 0);

    if (info->is_switch2_compatible)
    {
        POUND_LOG_INFO(&thread_logger, "Enabling Switch 2 compatibility mode");
    }

    return POUND_SUCCESS;
}

error_t
pound_title_loader_init(void)
{
    POUND_LOG_INFO(&thread_logger, "Title loader initialized");
    return POUND_SUCCESS;
}
