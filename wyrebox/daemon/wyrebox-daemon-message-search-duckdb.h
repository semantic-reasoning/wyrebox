#pragma once

#include "wyrebox-daemon-message-search-service.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_DAEMON_MESSAGE_SEARCH_DUCKDB_QUERY_ID "message-search"

/*
 * Creates a SEARCH service that evaluates requests against the materialized
 * catalog at @catalog_path, opened through wyrebox_duckdb_open_shared(). It
 * only reads the catalog.
 *
 * A successful search returns one final stream chunk whose query_id is
 * WYREBOX_DAEMON_MESSAGE_SEARCH_DUCKDB_QUERY_ID and whose bytes are the
 * matching visible UIDs of the selected mailbox or derived view, in ascending
 * order, each as ASCII decimal followed by '\n'. No match yields empty bytes.
 *
 * Subject and From criteria are substring matches that fold only ASCII
 * letters; other characters, including `%`, `_` and `\`, match literally.
 * Sender-domain criteria match the normalized sender domain exactly after
 * ASCII lowercasing. Date criteria compare the decoded Date header, and
 * messages without a decoded date never match them.
 *
 * A missing, hidden or unselectable mailbox or view fails with
 * G_IO_ERROR_NOT_FOUND, and a stale UIDVALIDITY with G_IO_ERROR_EXISTS.
 *
 * Returns: (transfer full): the service, or NULL with @error set.
 */
WyreboxDaemonMessageSearchService *
wyrebox_daemon_message_search_service_new_duckdb (const char *catalog_path,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
