#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Predicate name accepted by the deterministic fact pipeline.
   *
   * Ownership: caller owns and must clear with wyrebox_fact_record_clear().
   */
  char *predicate;

  /*
   * NULL-terminated argument vector. Each argument is an owned UTF-8 string.
   */
  char **args;

  /*
   * Provenance string identifying the extractor or source field.
   */
  char *source;

  /*
   * Deterministic confidence in parts per million. 1000000 means exact.
   */
  guint32 confidence_ppm;

  guint64 created_at_unix_us;
  guint64 retracted_at_unix_us;
} WyreboxFactRecord;

void wyrebox_fact_record_clear (WyreboxFactRecord *record);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxFactRecord,
    wyrebox_fact_record_clear)

gboolean wyrebox_fact_record_init (WyreboxFactRecord *record,
    const char *predicate,
    const char * const *args,
    const char *source,
    guint32 confidence_ppm,
    guint64 created_at_unix_us,
    GError **error);

gboolean wyrebox_fact_record_mark_retracted (WyreboxFactRecord *record,
    guint64 retracted_at_unix_us,
    GError **error);

char *wyrebox_fact_record_to_wirelog_fact (const WyreboxFactRecord *record,
    GError **error);

char *wyrebox_fact_record_array_to_wirelog_facts (GPtrArray *records,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
