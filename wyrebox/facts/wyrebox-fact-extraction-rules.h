#pragma once

#include "wyrebox-deterministic-fact-extractor.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_FACT_EXTRACTION_RULES (wyrebox_fact_extraction_rules_get_type())

G_DECLARE_FINAL_TYPE (WyreboxFactExtractionRules,
    wyrebox_fact_extraction_rules,
    WYREBOX,
    FACT_EXTRACTION_RULES,
    GObject)

/*
 * Returns: (transfer full): rules without dictionary or regex entries, which
 *   extract header facts only.
 */
WyreboxFactExtractionRules *wyrebox_fact_extraction_rules_new_empty (void);

/*
 * Loads dictionary and regex extraction rules from the key file at @path.
 *
 * Each group is one rule, applied in file order within its kind:
 *
 *   [dictionary:<rule-id>]  keys field, match, project
 *   [regex:<rule-id>]       keys field, predicate, pattern, capture_group
 *
 * capture_group is optional and defaults to 0. pattern is taken verbatim, so
 * backslashes need no escaping; other values follow key file string escapes.
 * Rule ids use letters, digits, '-', '_', or '.'. Fields, predicates,
 * patterns, and capture groups are checked with
 * wyrebox_deterministic_fact_rules_validate().
 *
 * An unreadable or invalid file fails with G_IO_ERROR_INVALID_DATA.
 *
 * Returns: (transfer full): loaded rules, or NULL with @error set.
 */
WyreboxFactExtractionRules *wyrebox_fact_extraction_rules_new_from_file (
    const char *path,
    GError **error);

/*
 * Extracts header facts and the facts of matching rules for @mail_id with
 * wyrebox_deterministic_fact_extract_from_metadata_with_rules().
 *
 * Returns: (transfer full) (element-type WyreboxFactRecord): extracted facts,
 *   or NULL with @error set.
 */
GPtrArray *wyrebox_fact_extraction_rules_extract (
    WyreboxFactExtractionRules *self,
    const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
