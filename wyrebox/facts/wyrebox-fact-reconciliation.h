#pragma once

#include "wyrebox-fact-record.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/*
 * Reconciles two active fact snapshots and returns change records.
 *
 * @previous_facts: (transfer none) (element-type WyreboxFactRecord): previous
 *   active snapshot.
 * @new_facts: (transfer none) (element-type WyreboxFactRecord): newly
 *   extracted active snapshot.
 *
 * Fact identity is predicate + args + source. Creation and retraction
 * timestamps are ignored for equality. The returned array owns copied
 * WyreboxFactRecord records. Retractions are emitted first in previous snapshot
 * order, followed by inserts in new snapshot order.
 *
 * Returns: (transfer full) (element-type WyreboxFactRecord): owned change
 *   records, or NULL with @error set.
 */
GPtrArray *wyrebox_fact_reconciliation_reconcile (
    GPtrArray *previous_facts,
    GPtrArray *new_facts,
    guint64 retracted_at_unix_us,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
