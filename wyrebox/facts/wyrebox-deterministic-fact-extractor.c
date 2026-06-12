#include "wyrebox-deterministic-fact-extractor.h"

#include <string.h>

#include <gio/gio.h>

#define WYREBOX_DETERMINISTIC_FACT_CONFIDENCE 1000000u

static void
fact_record_free (gpointer data)
{
  WyreboxFactRecord *record = data;

  if (record == NULL)
    return;

  wyrebox_fact_record_clear (record);
  g_free (record);
}

static gboolean
append_fact (GPtrArray *facts,
    const char *predicate,
    const char *arg0,
    const char *arg1,
    const char *source, guint64 created_at_unix_us, GError **error)
{
  const char *args[] = {
    arg0,
    arg1,
    NULL,
  };
  WyreboxFactRecord *record = NULL;

  record = g_new0 (WyreboxFactRecord, 1);
  if (!wyrebox_fact_record_init (record,
          predicate,
          args, source, WYREBOX_DETERMINISTIC_FACT_CONFIDENCE,
          created_at_unix_us, error)) {
    fact_record_free (record);
    return FALSE;
  }

  g_ptr_array_add (facts, record);
  return TRUE;
}

static char *
extract_domain_from_address_header (const char *value)
{
  const char *at = NULL;
  const char *domain_start = NULL;
  const char *domain_end = NULL;

  if (value == NULL)
    return NULL;

  at = strrchr (value, '@');
  if (at == NULL || at[1] == '\0')
    return NULL;

  domain_start = at + 1;
  domain_end = domain_start;
  while (*domain_end != '\0' &&
      *domain_end != '>' &&
      *domain_end != ',' && !g_ascii_isspace (*domain_end)) {
    domain_end++;
  }

  if (domain_end == domain_start)
    return NULL;

  return g_ascii_strdown (domain_start, domain_end - domain_start);
}

static gboolean
append_participant_if_present (GPtrArray *facts,
    const char *mail_id,
    const char *value,
    const char *source, guint64 created_at_unix_us, GError **error)
{
  if (value == NULL || *value == '\0')
    return TRUE;

  return append_fact (facts,
      "participant", mail_id, value, source, created_at_unix_us, error);
}

static gboolean
append_message_id_tokens (GPtrArray *facts,
    const char *predicate,
    const char *mail_id,
    const char *value,
    const char *source, guint64 created_at_unix_us, GError **error)
{
  const char *cursor = value;

  if (value == NULL || *value == '\0')
    return TRUE;

  while ((cursor = strchr (cursor, '<')) != NULL) {
    const char *end = strchr (cursor, '>');
    g_autofree char *message_id = NULL;

    if (end == NULL)
      break;

    if (end > cursor + 1) {
      message_id = g_strndup (cursor, (gsize) (end - cursor + 1));
      if (!append_fact (facts,
              predicate, mail_id, message_id, source, created_at_unix_us,
              error))
        return FALSE;
    }

    cursor = end + 1;
  }

  return TRUE;
}

static gboolean
rule_field_is_supported (const char *field)
{
  return g_strcmp0 (field, "subject") == 0 ||
      g_strcmp0 (field, "from") == 0 ||
      g_strcmp0 (field, "to") == 0 ||
      g_strcmp0 (field, "cc") == 0 || g_strcmp0 (field, "bcc") == 0;
}

static const char *
get_rule_field_value (const WyreboxEmlMetadata *metadata, const char *field)
{
  if (g_strcmp0 (field, "subject") == 0)
    return metadata->subject;
  if (g_strcmp0 (field, "from") == 0)
    return metadata->from;
  if (g_strcmp0 (field, "to") == 0)
    return metadata->to;
  if (g_strcmp0 (field, "cc") == 0)
    return metadata->cc;
  if (g_strcmp0 (field, "bcc") == 0)
    return metadata->bcc;

  return NULL;
}

static char *
casefold_for_match (const char *value)
{
  if (g_utf8_validate (value, -1, NULL))
    return g_utf8_casefold (value, -1);

  return g_ascii_strdown (value, -1);
}

static gboolean
field_contains_match_text (const char *field_value, const char *match_text)
{
  g_autofree char *folded_field = NULL;
  g_autofree char *folded_match = NULL;

  if (field_value == NULL || *field_value == '\0')
    return FALSE;

  folded_field = casefold_for_match (field_value);
  folded_match = casefold_for_match (match_text);

  return strstr (folded_field, folded_match) != NULL;
}

static gboolean
validate_dictionary_rules (const WyreboxDeterministicFactDictionaryRule *rules,
    gsize n_rules, GError **error)
{
  if (n_rules > 0 && rules == NULL) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "dictionary rules are NULL");
    return FALSE;
  }

  for (gsize i = 0; i < n_rules; i++) {
    if (rules[i].rule_id == NULL || rules[i].rule_id[0] == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT, "dictionary rule id is required");
      return FALSE;
    }

    if (!rule_field_is_supported (rules[i].field)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "unsupported dictionary rule field '%s'",
          rules[i].field != NULL ? rules[i].field : "(null)");
      return FALSE;
    }

    if (rules[i].match_text == NULL || rules[i].match_text[0] == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "dictionary rule match text is required");
      return FALSE;
    }

    if (rules[i].canonical_project_key == NULL ||
        rules[i].canonical_project_key[0] == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "dictionary rule canonical project key is required");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
append_dictionary_facts (GPtrArray *facts,
    const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us,
    const WyreboxDeterministicFactDictionaryRule *rules,
    gsize n_rules, GError **error)
{
  for (gsize i = 0; i < n_rules; i++) {
    const char *field_value = get_rule_field_value (metadata, rules[i].field);
    g_autofree char *source = NULL;

    if (!field_contains_match_text (field_value, rules[i].match_text))
      continue;

    source = g_strdup_printf ("dictionary:%s:%s",
        rules[i].field, rules[i].rule_id);
    if (!append_fact (facts,
            "project_keyword",
            mail_id,
            rules[i].canonical_project_key, source, created_at_unix_us, error))
      return FALSE;
  }

  return TRUE;
}

GPtrArray *
wyrebox_deterministic_fact_extract_from_metadata_with_dictionary (const char
    *mail_id, const WyreboxEmlMetadata *metadata, guint64 created_at_unix_us,
    const WyreboxDeterministicFactDictionaryRule *rules, gsize n_rules,
    GError **error)
{
  g_autoptr (GPtrArray) facts = NULL;
  g_autofree char *sender_domain = NULL;

  g_return_val_if_fail (metadata != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (mail_id == NULL || *mail_id == '\0') {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "mail_id is required");
    return NULL;
  }

  if (created_at_unix_us == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact extraction timestamp is required");
    return NULL;
  }

  if (!validate_dictionary_rules (rules, n_rules, error))
    return NULL;

  facts = g_ptr_array_new_with_free_func (fact_record_free);

  if (metadata->message_id != NULL && metadata->message_id[0] != '\0') {
    if (!append_fact (facts,
            "message_id",
            mail_id,
            metadata->message_id,
            "header:message-id", created_at_unix_us, error))
      return NULL;
  }

  sender_domain = extract_domain_from_address_header (metadata->from);
  if (sender_domain != NULL) {
    if (!append_fact (facts,
            "sender_domain",
            mail_id, sender_domain, "header:from", created_at_unix_us, error))
      return NULL;
  }

  if (!append_participant_if_present (facts,
          mail_id, metadata->from, "header:from", created_at_unix_us, error))
    return NULL;

  if (!append_participant_if_present (facts,
          mail_id, metadata->to, "header:to", created_at_unix_us, error))
    return NULL;

  if (!append_participant_if_present (facts,
          mail_id, metadata->cc, "header:cc", created_at_unix_us, error))
    return NULL;

  if (!append_participant_if_present (facts,
          mail_id, metadata->bcc, "header:bcc", created_at_unix_us, error))
    return NULL;

  if (metadata->date != NULL && metadata->date[0] != '\0') {
    if (!append_fact (facts,
            "sent_at",
            mail_id, metadata->date, "header:date", created_at_unix_us, error))
      return NULL;
  }

  if (!append_message_id_tokens (facts,
          "replies_to",
          mail_id,
          metadata->in_reply_to,
          "header:in-reply-to", created_at_unix_us, error))
    return NULL;

  if (!append_message_id_tokens (facts,
          "references",
          mail_id,
          metadata->references, "header:references", created_at_unix_us, error))
    return NULL;

  if (!append_dictionary_facts (facts,
          mail_id, metadata, created_at_unix_us, rules, n_rules, error))
    return NULL;

  return g_steal_pointer (&facts);
}

GPtrArray *
wyrebox_deterministic_fact_extract_from_metadata (const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us, GError **error)
{
  return
      wyrebox_deterministic_fact_extract_from_metadata_with_dictionary (mail_id,
      metadata, created_at_unix_us, NULL, 0, error);
}
