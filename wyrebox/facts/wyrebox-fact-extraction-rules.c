#include "wyrebox-fact-extraction-rules.h"

#include <gio/gio.h>

#include <string.h>

struct _WyreboxFactExtractionRules
{
    GObject parent_instance;

    GArray *dictionary_rules;
    GArray *regex_rules;
    GPtrArray *strings;
};

G_DEFINE_TYPE (WyreboxFactExtractionRules, wyrebox_fact_extraction_rules,
    G_TYPE_OBJECT);

static void
wyrebox_fact_extraction_rules_finalize (GObject *object)
{
    WyreboxFactExtractionRules *self = WYREBOX_FACT_EXTRACTION_RULES (object);

    g_clear_pointer (&self->dictionary_rules, g_array_unref);
    g_clear_pointer (&self->regex_rules, g_array_unref);
    g_clear_pointer (&self->strings, g_ptr_array_unref);

    G_OBJECT_CLASS (wyrebox_fact_extraction_rules_parent_class)->finalize (
        object);
}

static void
wyrebox_fact_extraction_rules_class_init (WyreboxFactExtractionRulesClass
    *klass)
{
    G_OBJECT_CLASS (klass)->finalize = wyrebox_fact_extraction_rules_finalize;
}

static void
wyrebox_fact_extraction_rules_init (WyreboxFactExtractionRules *self)
{
    self->dictionary_rules = g_array_new (FALSE, TRUE,
            sizeof (WyreboxDeterministicFactDictionaryRule));
    self->regex_rules = g_array_new (FALSE, TRUE,
            sizeof (WyreboxDeterministicFactRegexRule));
    self->strings = g_ptr_array_new_with_free_func (g_free);
}

WyreboxFactExtractionRules *
wyrebox_fact_extraction_rules_new_empty (void)
{
    return g_object_new (WYREBOX_TYPE_FACT_EXTRACTION_RULES, NULL);
}

static gboolean
rule_id_is_valid (const char *rule_id)
{
    if (*rule_id == '\0')
        return FALSE;

    for (const char *cursor = rule_id; *cursor != '\0'; cursor++) {
        if (!g_ascii_isalnum (*cursor) && *cursor != '-' && *cursor != '_' &&
            *cursor != '.')
            return FALSE;
    }

    return TRUE;
}

static const char *
keep_string (WyreboxFactExtractionRules *self, char *value)
{
    g_ptr_array_add (self->strings, value);
    return value;
}

static gboolean
check_group_keys (GKeyFile *key_file, const char *path, const char *group,
    const char *const *allowed, GError **error)
{
    g_auto (GStrv) keys = g_key_file_get_keys (key_file, group, NULL, NULL);

    for (guint i = 0; keys != NULL && keys[i] != NULL; i++) {
        if (!g_strv_contains (allowed, keys[i])) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_DATA,
                "extraction rules file '%s' group %s has unknown key '%s'",
                path, group, keys[i]);
            return FALSE;
        }
    }

    return TRUE;
}

static char *
read_required (GKeyFile *key_file, const char *path, const char *group,
    const char *key, gboolean verbatim, GError **error)
{
    char *value = NULL;

    if (g_key_file_has_key (key_file, group, key, NULL))
        value = verbatim ? g_key_file_get_value (key_file, group, key, NULL) :
            g_key_file_get_string (key_file, group, key, NULL);

    if (value == NULL || value[0] == '\0') {
        g_free (value);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "extraction rules file '%s' group %s is missing %s", path, group,
            key);
        return NULL;
    }

    return value;
}

static gboolean
add_dictionary_rule (WyreboxFactExtractionRules *self, GKeyFile *key_file,
    const char *path, const char *group, const char *rule_id, GError **error)
{
    static const char *const allowed[] = { "field", "match", "project", NULL };
    WyreboxDeterministicFactDictionaryRule rule = { 0 };
    char *field = NULL;
    char *match = NULL;
    char *project = NULL;

    if (!check_group_keys (key_file, path, group, allowed, error))
        return FALSE;

    if ((field = read_required (key_file, path, group, "field", FALSE,
        error)) == NULL)
        return FALSE;
    rule.field = keep_string (self, field);
    if ((match = read_required (key_file, path, group, "match", FALSE,
        error)) == NULL)
        return FALSE;
    rule.match_text = keep_string (self, match);
    if ((project = read_required (key_file, path, group, "project", FALSE,
        error)) == NULL)
        return FALSE;
    rule.canonical_project_key = keep_string (self, project);
    rule.rule_id = keep_string (self, g_strdup (rule_id));

    g_array_append_val (self->dictionary_rules, rule);
    return TRUE;
}

static gboolean
parse_capture_group (GKeyFile *key_file, const char *path, const char *group,
    guint *out_capture_group, GError **error)
{
    g_autofree char *value = NULL;
    guint64 parsed = 0;

    *out_capture_group = 0;
    if (!g_key_file_has_key (key_file, group, "capture_group", NULL))
        return TRUE;

    value = g_key_file_get_value (key_file, group, "capture_group", NULL);
    if (value == NULL || !g_ascii_string_to_unsigned (value, 10, 0, G_MAXINT,
        &parsed, NULL)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "extraction rules file '%s' group %s capture_group must be a "
            "non-negative integer", path, group);
        return FALSE;
    }

    *out_capture_group = (guint)parsed;
    return TRUE;
}

static gboolean
add_regex_rule (WyreboxFactExtractionRules *self, GKeyFile *key_file,
    const char *path, const char *group, const char *rule_id, GError **error)
{
    static const char *const allowed[] = {
        "field", "predicate", "pattern", "capture_group", NULL
    };
    WyreboxDeterministicFactRegexRule rule = { 0 };
    char *field = NULL;
    char *predicate = NULL;
    char *pattern = NULL;

    if (!check_group_keys (key_file, path, group, allowed, error))
        return FALSE;

    if ((field = read_required (key_file, path, group, "field", FALSE,
        error)) == NULL)
        return FALSE;
    rule.field = keep_string (self, field);
    if ((predicate = read_required (key_file, path, group, "predicate", FALSE,
        error)) == NULL)
        return FALSE;
    rule.predicate = keep_string (self, predicate);
    if ((pattern = read_required (key_file, path, group, "pattern", TRUE,
        error)) == NULL)
        return FALSE;
    rule.pattern = keep_string (self, pattern);
    if (!parse_capture_group (key_file, path, group, &rule.capture_group,
        error))
        return FALSE;
    rule.rule_id = keep_string (self, g_strdup (rule_id));

    g_array_append_val (self->regex_rules, rule);
    return TRUE;
}

static gboolean
add_rule_group (WyreboxFactExtractionRules *self, GKeyFile *key_file,
    const char *path, const char *group, GError **error)
{
    const char *separator = strchr (group, ':');
    g_autofree char *kind = NULL;
    const char *rule_id = NULL;

    if (separator == NULL ||
        (!g_str_has_prefix (group, "dictionary:") &&
        !g_str_has_prefix (group, "regex:"))) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "extraction rules file '%s' has unsupported group '%s'; use "
            "[dictionary:<id>] or [regex:<id>]", path, group);
        return FALSE;
    }

    kind = g_strndup (group, (gsize)(separator - group));
    rule_id = separator + 1;
    if (!rule_id_is_valid (rule_id)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "extraction rules file '%s' has invalid rule id '%s'; use "
            "letters, digits, '-', '_', or '.'", path, rule_id);
        return FALSE;
    }

    if (g_strcmp0 (kind, "dictionary") == 0)
        return add_dictionary_rule (self, key_file, path, group, rule_id,
                   error);

    return add_regex_rule (self, key_file, path, group, rule_id, error);
}

WyreboxFactExtractionRules *
wyrebox_fact_extraction_rules_new_from_file (const char *path, GError **error)
{
    g_autoptr (WyreboxFactExtractionRules) self = NULL;
    g_autoptr (GKeyFile) key_file = g_key_file_new ();
    g_autoptr (GError) local_error = NULL;
    g_autofree char *contents = NULL;
    g_auto (GStrv) groups = NULL;
    gsize length = 0;

    g_return_val_if_fail (path != NULL, NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (!g_file_get_contents (path, &contents, &length, &local_error)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "extraction rules file '%s' cannot be read: %s", path,
            local_error->message);
        return NULL;
    }

    if (!g_key_file_load_from_data (key_file, contents, length,
        G_KEY_FILE_NONE, &local_error)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "extraction rules file '%s' cannot be parsed: %s", path,
            local_error->message);
        return NULL;
    }

    self = wyrebox_fact_extraction_rules_new_empty ();
    groups = g_key_file_get_groups (key_file, NULL);
    for (guint i = 0; groups[i] != NULL; i++) {
        if (!add_rule_group (self, key_file, path, groups[i], error))
            return NULL;
    }

    if (!wyrebox_deterministic_fact_rules_validate (
            (const WyreboxDeterministicFactDictionaryRule *)
            self->dictionary_rules->data, self->dictionary_rules->len,
            (const WyreboxDeterministicFactRegexRule *)
            self->regex_rules->data, self->regex_rules->len,
            &local_error)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "extraction rules file '%s' is invalid: %s", path,
            local_error->message);
        return NULL;
    }

    return g_steal_pointer (&self);
}

GPtrArray *
wyrebox_fact_extraction_rules_extract (WyreboxFactExtractionRules *self,
    const char *mail_id, const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us, GError **error)
{
    g_return_val_if_fail (WYREBOX_IS_FACT_EXTRACTION_RULES (self), NULL);

    return wyrebox_deterministic_fact_extract_from_metadata_with_rules (mail_id,
               metadata, created_at_unix_us,
               (const WyreboxDeterministicFactDictionaryRule *)
               self->dictionary_rules->data, self->dictionary_rules->len,
               (const WyreboxDeterministicFactRegexRule *)
               self->regex_rules->data, self->regex_rules->len, error);
}
