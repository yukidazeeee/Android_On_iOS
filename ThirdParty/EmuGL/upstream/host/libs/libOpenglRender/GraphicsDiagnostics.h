#ifndef ANDROIDEMU_GRAPHICS_DIAGNOSTICS_H
#define ANDROIDEMU_GRAPHICS_DIAGNOSTICS_H

#include <stdlib.h>
inline bool aeGraphicsDiagEnabled(const char *name) {
    const char *value = getenv(name);
    return value && value[0] == '1' && value[1] == '\0';
}

#endif
