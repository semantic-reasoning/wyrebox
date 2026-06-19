/*
 * Copyright (C) 2026
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#include "wyrebox-journal-reader.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-schema-migration.h"

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_RECOVERY_MATERIALIZED_STATE_DECIDER \
  (wyrebox_recovery_materialized_state_decider_get_type ())

G_DECLARE_FINAL_TYPE (WyreboxRecoveryMaterializedStateDecider,
    wyrebox_recovery_materialized_state_decider,
    WYREBOX,
    RECOVERY_MATERIALIZED_STATE_DECIDER,
    GObject)

typedef enum
{
  WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_USE_MATERIALIZED_STATE,
  WYREBOX_RECOVERY_MATERIALIZED_STATE_DECISION_REBUILD_MATERIALIZED_STATE,
} WyreboxRecoveryMaterializedStateDecision;

WyreboxRecoveryMaterializedStateDecider *
wyrebox_recovery_materialized_state_decider_new (void);

gboolean wyrebox_recovery_materialized_state_decider_decide (
    WyreboxRecoveryMaterializedStateDecider *self,
    const gchar *object_store_root,
    WyreboxLocalObjectStore *object_store,
    const gchar *catalog_path,
    WyreboxJournalReader *journal_reader,
    const WyreboxSchemaMigrationMetadataState *metadata_state,
    WyreboxRecoveryMaterializedStateDecision *out_decision,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
