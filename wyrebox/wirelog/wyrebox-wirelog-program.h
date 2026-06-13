#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_WIRELOG_PROGRAM (wyrebox_wirelog_program_get_type ())
G_DECLARE_FINAL_TYPE (WyreboxWirelogProgram,
    wyrebox_wirelog_program,
    WYREBOX,
    WIRELOG_PROGRAM,
    GObject)

WyreboxWirelogProgram *wyrebox_wirelog_program_new_from_source (
    const char *source,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
