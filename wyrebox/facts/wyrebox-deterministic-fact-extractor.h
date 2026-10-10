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

typedef struct
{
  const char *field;
  const char *rule_id;
  const char *predicate;
  const char *pattern;
  guint capture_group;
} WyreboxDeterministicFactRegexRule;

/*
 * Extracts deterministic header-derived facts from parsed EML metadata.
 * RFC 2047 encoded words are decoded for Subject, From, To, Cc, and Bcc
 * extraction and rule matching. Address fields emit one normalized
 * participant fact per valid address; display names are emitted separately.
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
 * case-insensitive substring check over the decoded working value; parsed
 * metadata remains unchanged.
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

/*
 * Extracts deterministic header-derived facts and then appends candidate facts
 * for matching caller-supplied regex rules.
 *
 * @rules: (nullable) (array length=n_rules): caller-owned in-memory regex
 *   rules.
 *
 * Supported rule fields are subject, from, to, cc, and bcc. Supported
 * predicates are amount_candidate, date_candidate, and reference_candidate.
 * capture_group 0 emits the full match; positive capture_group values emit the
 * selected capture. Unmatched or empty captures emit no fact.
 *
 * Returns: (transfer full) (element-type WyreboxFactRecord): fact records owned
 *   by the returned array.
 */
GPtrArray *wyrebox_deterministic_fact_extract_from_metadata_with_regex (
    const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us,
    const WyreboxDeterministicFactRegexRule *rules,
    gsize n_rules,
    GError **error);

/*
 * Checks @dictionary_rules and @regex_rules as
 * wyrebox_deterministic_fact_extract_from_metadata_with_rules() would before
 * extracting, including that every regex pattern compiles and every capture
 * group exists. Invalid rules fail with G_IO_ERROR_INVALID_ARGUMENT.
 */
gboolean wyrebox_deterministic_fact_rules_validate (
    const WyreboxDeterministicFactDictionaryRule *dictionary_rules,
    gsize n_dictionary_rules,
    const WyreboxDeterministicFactRegexRule *regex_rules,
    gsize n_regex_rules,
    GError **error);

/*
 * Extracts deterministic header-derived facts, dictionary project keywords,
 * and regex candidate facts in that order.
 *
 * Returns: (transfer full) (element-type WyreboxFactRecord): fact records owned
 *   by the returned array.
 */
GPtrArray *wyrebox_deterministic_fact_extract_from_metadata_with_rules (
    const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us,
    const WyreboxDeterministicFactDictionaryRule *dictionary_rules,
    gsize n_dictionary_rules,
    const WyreboxDeterministicFactRegexRule *regex_rules,
    gsize n_regex_rules,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
