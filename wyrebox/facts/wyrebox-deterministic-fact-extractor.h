#pragma once

#include "wyrebox-eml-metadata.h"
#include "wyrebox-fact-record.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Extracts deterministic header-derived facts from parsed EML metadata.
 *
 * Returns: (transfer full) (element-type WyreboxFactRecord): fact records owned
 *   by the returned array.
 */
GPtrArray *wyrebox_deterministic_fact_extract_from_metadata (
    const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
