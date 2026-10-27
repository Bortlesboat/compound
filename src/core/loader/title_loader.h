#ifndef POUND_LOADER_TITLE_LOADER_H
#define POUND_LOADER_TITLE_LOADER_H

#include "attributes.h"
#include "errors.h"
#include "fs/fs_reader.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    POUND_TITLE_UNKNOWN = 0,
    POUND_TITLE_NRO,
    POUND_TITLE_NSO,
    POUND_TITLE_NCA,
    POUND_TITLE_NSP,
    POUND_TITLE_XCI,
    POUND_TITLE_HAC,
} pound_title_type_t;

typedef struct
{
    pound_title_type_t type;
    char path[512];
    bool is_switch2_compatible;
} pound_title_info_t;

/// Detect title type from file
error_t pound_title_detect(const char *path, pound_title_info_t *info);

/// Load a title (NRO/NSP/XCI/NCA). For Switch 2 titles, enable extended compatibility mode.
error_t pound_title_load(const pound_title_info_t *info);

/// Initialize loader subsystem
error_t pound_title_loader_init(void);

#ifdef __cplusplus
}
#endif

#endif // POUND_LOADER_TITLE_LOADER_H
