#pragma once

#include "wyrebox-fact-record.h"

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

GPtrArray *wyrebox_fact_journal_snapshot_load_active (
    const char *journal_root_dir,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
