#pragma once

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_rfc5322_date_parse_unix_us (const gchar *value,
    gint64 *out_unix_us);

G_END_DECLS
/* *INDENT-ON* */
