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

GPtrArray *
wyrebox_deterministic_fact_extract_from_metadata (const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us, GError **error)
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

  facts = g_ptr_array_new_with_free_func (fact_record_free);

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

  return g_steal_pointer (&facts);
}
