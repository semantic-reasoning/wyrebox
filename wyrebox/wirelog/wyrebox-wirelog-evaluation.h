#pragma once

#include "wyrebox-wirelog-program.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_WIRELOG_EVALUATION \
  (wyrebox_wirelog_evaluation_get_type ())
G_DECLARE_FINAL_TYPE (WyreboxWirelogEvaluation,
    wyrebox_wirelog_evaluation,
    WYREBOX,
    WIRELOG_EVALUATION,
    GObject)

gboolean wyrebox_wirelog_program_evaluate (
    WyreboxWirelogProgram *program,
    WyreboxWirelogEvaluation **out_eval,
    GError **error);

gboolean wyrebox_wirelog_evaluation_get_relation_cardinality (
    WyreboxWirelogEvaluation *self,
    const char *relation_name,
    guint64 *out_cardinality,
    GError **error);

gboolean wyrebox_wirelog_evaluation_export_relation_csv (
    WyreboxWirelogEvaluation *self,
    const char *relation_name,
    char **out_csv,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
