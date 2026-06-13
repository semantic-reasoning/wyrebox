#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

char *wyrebox_wirelog_rule_version_hash (
    const char *rules_source,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
