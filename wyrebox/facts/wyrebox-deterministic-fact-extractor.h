#pragma once

#include "wyrebox-eml-metadata.h"
#include "wyrebox-fact-record.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  const char *field;
  const char *rule_id;
  const char *match_text;
  const char *canonical_project_key;
} WyreboxDeterministicFactDictionaryRule;

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

/*
 * Extracts deterministic header-derived facts and then appends
 * project_keyword(mail_id, canonical_project_key) facts for matching
 * caller-supplied dictionary rules.
 *
 * @rules: (nullable) (array length=n_rules): caller-owned in-memory
 *   dictionary rules.
 *
 * Supported rule fields are subject, from, to, cc, and bcc. Matching is a
 * case-insensitive substring check over the selected parsed metadata field.
 *
 * Returns: (transfer full) (element-type WyreboxFactRecord): fact records owned
 *   by the returned array.
 */
GPtrArray *wyrebox_deterministic_fact_extract_from_metadata_with_dictionary (
    const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us,
    const WyreboxDeterministicFactDictionaryRule *rules,
    gsize n_rules,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
