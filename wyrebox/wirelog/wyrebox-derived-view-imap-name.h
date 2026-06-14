#pragma once

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_derived_view_imap_name_validate_stored (
    const gchar *imap_name,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
