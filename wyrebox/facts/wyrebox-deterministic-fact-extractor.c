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
append_fact_with_args (GPtrArray *facts,
    const char *predicate,
    const char *const *args,
    const char *source, guint64 created_at_unix_us, GError **error)
{
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

    return append_fact_with_args (facts, predicate, args, source,
               created_at_unix_us, error);
}

static gboolean
append_participant_display_name (GPtrArray *facts,
    const char *mail_id,
    const char *address,
    const char *display_name,
    const char *source, guint64 created_at_unix_us, GError **error)
{
    const char *args[] = {
        mail_id,
        address,
        display_name,
        NULL,
    };

    return append_fact_with_args (facts, "participant_display_name", args,
               source, created_at_unix_us, error);
}

static gint
hex_digit_value (char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;

    return -1;
}

static gint
base64_digit_value (char value)
{
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return value - 'a' + 26;
    if (value >= '0' && value <= '9')
        return value - '0' + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;

    return -1;
}

static gboolean
decode_base64_word (const char *encoded, GByteArray *decoded)
{
    gsize encoded_len = strlen (encoded);
    gsize decoded_len = 0;
    guint padding = 0;

    if (encoded_len == 0 || encoded_len % 4 != 0)
        return FALSE;

    for (gsize i = 0; i < encoded_len; i++) {
        char c = encoded[i];

        if (c == '=') {
            padding++;
            if (i < encoded_len - 2 || padding > 2)
                return FALSE;
        } else if (padding != 0 ||
            !(g_ascii_isalnum (c) || c == '+' || c == '/')) {
            return FALSE;
        }
    }

    /* RFC 4648 requires unused bits in a padded quantum to be zero. */
    if ((padding == 2 &&
        (base64_digit_value (encoded[encoded_len - 3]) & 0x0f) != 0) ||
        (padding == 1 &&
        (base64_digit_value (encoded[encoded_len - 2]) & 0x03) != 0))
        return FALSE;

    g_autofree guchar *bytes = g_base64_decode (encoded, &decoded_len);
    if (bytes == NULL || decoded_len == 0)
        return FALSE;

    g_byte_array_append (decoded, bytes, decoded_len);
    return TRUE;
}

static gboolean
decode_quoted_printable_word (const char *encoded, GByteArray *decoded)
{
    for (const char *cursor = encoded; *cursor != '\0'; cursor++) {
        guint8 byte = 0;

        if (*cursor == '_') {
            byte = ' ';
        } else if (*cursor == '=') {
            gint high = hex_digit_value (cursor[1]);
            gint low = cursor[1] != '\0' ? hex_digit_value (cursor[2]) : -1;

            if (high < 0 || low < 0)
                return FALSE;
            byte = (guint8)((high << 4) | low);
            cursor += 2;
        } else {
            if ((guchar)*cursor < 0x21 || (guchar)*cursor > 0x7e ||
                *cursor == '?')
                return FALSE;
            byte = (guint8)*cursor;
        }

        g_byte_array_append (decoded, &byte, 1);
    }

    return TRUE;
}

static char *
decode_rfc2047_word (const char *charset,
    char encoding,
    const char *encoded)
{
    g_autoptr (GByteArray) bytes = g_byte_array_new ();
    g_autoptr (GError) error = NULL;
    g_autofree char *converted = NULL;
    gsize bytes_read = 0;
    gsize bytes_written = 0;

    if (encoding == 'B' || encoding == 'b') {
        if (!decode_base64_word (encoded, bytes))
            return NULL;
    } else if (encoding == 'Q' || encoding == 'q') {
        if (!decode_quoted_printable_word (encoded, bytes))
            return NULL;
    } else {
        return NULL;
    }

    converted = g_convert ((const char *)bytes->data, bytes->len, "UTF-8",
            charset, &bytes_read, &bytes_written, &error);
    if (converted == NULL || bytes_read != bytes->len ||
        memchr (converted, '\0', bytes_written) != NULL ||
        !g_utf8_validate (converted, (gssize)bytes_written, NULL))
        return NULL;

    return g_strndup (converted, bytes_written);
}

static char *
decode_header_value (const char *value)
{
    g_autoptr (GString) output = NULL;
    const char *cursor = value;

    if (value == NULL)
        return NULL;

    output = g_string_new (NULL);
    while (*cursor != '\0') {
        const char *start = strstr (cursor, "=?");
        const char *charset_end = NULL;
        const char *encoding_end = NULL;
        const char *word_end = NULL;
        g_autofree char *charset = NULL;
        g_autofree char *encoded = NULL;
        g_autofree char *decoded = NULL;

        if (start == NULL) {
            g_string_append (output, cursor);
            break;
        }

        g_string_append_len (output, cursor, start - cursor);
        charset_end = strchr (start + 2, '?');
        encoding_end = charset_end != NULL ?
            strchr (charset_end + 1, '?') : NULL;
        word_end = encoding_end != NULL ?
            strstr (encoding_end + 1, "?=") : NULL;

        if (charset_end == NULL || charset_end == start + 2 ||
            encoding_end != charset_end + 2 || word_end == NULL) {
            g_string_append_c (output, *start);
            cursor = start + 1;
            continue;
        }

        charset = g_strndup (start + 2, charset_end - start - 2);
        encoded = g_strndup (encoding_end + 1, word_end - encoding_end - 1);
        decoded = decode_rfc2047_word (charset, charset_end[1], encoded);
        if (decoded == NULL)
            return g_strdup (value);

        g_string_append (output, decoded);
        cursor = word_end + 2;

        {
            const char *next_word = cursor;

            while (g_ascii_isspace (*next_word))
                next_word++;
            if (g_str_has_prefix (next_word, "=?"))
                cursor = next_word;
        }
    }

    return g_string_free (g_steal_pointer (&output), FALSE);
}

typedef struct
{
    char *address;
    char *display_name;
} ParsedAddress;

static void
parsed_address_free (gpointer data)
{
    ParsedAddress *address = data;

    if (address == NULL)
        return;

    g_free (address->address);
    g_free (address->display_name);
    g_free (address);
}

static const char *
find_encoded_word_end (const char *start);

static char *
remove_comments (const char *value, gboolean *out_valid)
{
    g_autoptr (GString) output = g_string_new (NULL);
    guint comment_depth = 0;
    gboolean quoted = FALSE;
    gboolean escaped = FALSE;

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        char c = *cursor;

        if (c == '=' && cursor[1] == '?') {
            const char *word_end = find_encoded_word_end (cursor);

            if (word_end != NULL) {
                if (comment_depth == 0)
                    g_string_append_len (output, cursor, word_end - cursor);
                cursor = word_end - 1;
                continue;
            }
        }

        if (comment_depth > 0) {
            if (escaped) {
                escaped = FALSE;
            } else if (c == '\\') {
                escaped = TRUE;
            } else if (c == '(') {
                comment_depth++;
            } else if (c == ')') {
                comment_depth--;
            }
            continue;
        }

        if (escaped) {
            g_string_append_c (output, c);
            escaped = FALSE;
        } else if (quoted && c == '\\') {
            g_string_append_c (output, c);
            escaped = TRUE;
        } else if (c == '"') {
            quoted = !quoted;
            g_string_append_c (output, c);
        } else if (!quoted && c == '(') {
            comment_depth = 1;
            g_string_append_c (output, ' ');
        } else {
            g_string_append_c (output, c);
        }
    }

    if (out_valid != NULL)
        *out_valid = comment_depth == 0 && !escaped;

    return g_string_free (g_steal_pointer (&output), FALSE);
}

static const char *
find_address_separator (const char *value)
{
    const char *at = NULL;
    gboolean quoted = FALSE;
    gboolean escaped = FALSE;
    gboolean domain_literal = FALSE;

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        char c = *cursor;

        if (escaped) {
            escaped = FALSE;
        } else if (quoted && c == '\\') {
            escaped = TRUE;
        } else if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && c == '@' && !domain_literal) {
            if (at != NULL)
                return NULL;
            at = cursor;
        } else if (!quoted && at != NULL && c == '[') {
            if (domain_literal)
                return NULL;
            domain_literal = TRUE;
        } else if (!quoted && at != NULL && c == ']') {
            if (!domain_literal)
                return NULL;
            domain_literal = FALSE;
        } else if (!quoted && g_ascii_isspace (c)) {
            return NULL;
        }
    }

    if (quoted || escaped || domain_literal)
        return NULL;

    return at;
}

static gboolean
local_part_is_valid (const char *local, gsize length)
{
    if (length == 0 ||
        !g_utf8_validate (local, (gssize)length, NULL))
        return FALSE;

    if (local[0] == '"') {
        gboolean escaped = FALSE;

        if (length < 2 || local[length - 1] != '"')
            return FALSE;

        for (gsize i = 1; i + 1 < length; i++) {
            guchar c = (guchar)local[i];

            if (escaped) {
                escaped = FALSE;
            } else if (c == '\\') {
                escaped = TRUE;
            } else if (c == '"' || c == '\r' || c == '\n' || c < 0x20 ||
                c == 0x7f) {
                return FALSE;
            }
        }

        return !escaped;
    }

    if (local[0] == '.' || local[length - 1] == '.')
        return FALSE;

    for (gsize i = 0; i < length; i++) {
        guchar c = (guchar)local[i];

        if (c == '.') {
            if (i > 0 && local[i - 1] == '.')
                return FALSE;
        } else if (c < 0x80 &&
            !(g_ascii_isalnum (c) || strchr (
                "!#$%&'*+-/=?^_`{|}~", c) != NULL)) {
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
domain_is_valid (const char *domain)
{
    gsize length = strlen (domain);

    if (length == 0)
        return FALSE;

    if (domain[0] == '[') {
        if (length < 3 || domain[length - 1] != ']')
            return FALSE;

        for (gsize i = 1; i + 1 < length; i++) {
            guchar c = (guchar)domain[i];

            if (c <= 0x20 || c == 0x7f || c == '[' || c == ']')
                return FALSE;
        }

        return TRUE;
    }

    if (!g_utf8_validate (domain, -1, NULL))
        return FALSE;

    if (domain[0] == '.' || domain[length - 1] == '.')
        return FALSE;

    gboolean label_has_character = FALSE;
    gboolean previous_was_hyphen = FALSE;
    for (gsize i = 0; i < length; i++) {
        guchar c = (guchar)domain[i];

        if (c == '.') {
            if (!label_has_character || previous_was_hyphen)
                return FALSE;
            label_has_character = FALSE;
            previous_was_hyphen = FALSE;
        } else if (c == '-') {
            if (!label_has_character)
                return FALSE;
            label_has_character = TRUE;
            previous_was_hyphen = TRUE;
        } else if (c >= 0x80 || g_ascii_isalnum (c)) {
            label_has_character = TRUE;
            previous_was_hyphen = FALSE;
        } else {
            return FALSE;
        }
    }

    if (!label_has_character || previous_was_hyphen)
        return FALSE;

    {
        g_autofree char *ascii_domain = g_hostname_to_ascii (domain);

        return ascii_domain != NULL;
    }
}

static char *
normalize_address (const char *value)
{
    g_autofree char *trimmed = g_strdup (value);
    const char *at = NULL;

    g_strstrip (trimmed);
    at = find_address_separator (trimmed);

    if (at == NULL || at == trimmed || at[1] == '\0')
        return NULL;

    if (!g_utf8_validate (at + 1, -1, NULL))
        return NULL;

    {
        gsize local_len = (gsize)(at - trimmed);
        g_autofree char *domain = g_utf8_strdown (at + 1, -1);

        if (!local_part_is_valid (trimmed, local_len) ||
            !domain_is_valid (domain))
            return NULL;

        return g_strdup_printf ("%.*s@%s", (gint)local_len, trimmed, domain);
    }
}

static gboolean
phrase_atext_is_valid (guchar value)
{
    return value >= 0x80 ||
           g_ascii_isalnum (value) ||
           value == '.' ||
           strchr ("!#$%&'*+-/=?^_`{|}~", value) != NULL;
}

static gboolean
display_name_phrase_is_valid (const char *value)
{
    const char *cursor = value;
    gboolean has_word = FALSE;

    if (!g_utf8_validate (value, -1, NULL))
        return FALSE;

    while (*cursor != '\0') {
        gboolean had_whitespace = FALSE;

        while (g_ascii_isspace (*cursor)) {
            had_whitespace = TRUE;
            cursor++;
        }
        if (*cursor == '\0')
            break;
        if (has_word && !had_whitespace)
            return FALSE;

        if (*cursor == '"') {
            gboolean escaped = FALSE;
            gboolean closed = FALSE;

            cursor++;
            while (*cursor != '\0') {
                guchar c = (guchar)*cursor++;

                if (escaped) {
                    escaped = FALSE;
                } else if (c == '\\') {
                    escaped = TRUE;
                } else if (c == '"') {
                    closed = TRUE;
                    break;
                } else if (c < 0x20 || c == 0x7f) {
                    return FALSE;
                }
            }

            if (!closed || escaped)
                return FALSE;
        } else if (g_str_has_prefix (cursor, "=?")) {
            const char *word_end = find_encoded_word_end (cursor);

            if (word_end == NULL)
                return FALSE;
            cursor = word_end;
        } else {
            const char *word_start = cursor;

            while (*cursor != '\0' && !g_ascii_isspace (*cursor)) {
                if (!phrase_atext_is_valid ((guchar)*cursor))
                    return FALSE;
                cursor++;
            }
            if (cursor == word_start)
                return FALSE;
        }

        has_word = TRUE;
    }

    return has_word;
}

static char *
display_name_from_segment (const char *segment, char **address_text)
{
    gboolean comments_valid = FALSE;
    g_autofree char *without_comments = remove_comments (segment,
            &comments_valid);
    const char *angle_start = NULL;
    const char *angle_end = NULL;
    gboolean quoted = FALSE;
    gboolean escaped = FALSE;

    if (!comments_valid) {
        *address_text = g_strdup ("");
        return NULL;
    }

    for (const char *cursor = without_comments; *cursor != '\0'; cursor++) {
        if (*cursor == '=' && cursor[1] == '?') {
            const char *word_end = find_encoded_word_end (cursor);

            if (word_end != NULL) {
                cursor = word_end - 1;
                continue;
            }
        }

        if (escaped) {
            escaped = FALSE;
        } else if (quoted && *cursor == '\\') {
            escaped = TRUE;
        } else if (*cursor == '"') {
            quoted = !quoted;
        } else if (!quoted && *cursor == '<') {
            angle_start = cursor;
            break;
        }
    }

    if (angle_start == NULL) {
        *address_text = g_strdup (without_comments);
        return NULL;
    }

    quoted = FALSE;
    escaped = FALSE;
    for (const char *cursor = angle_start + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '=' && cursor[1] == '?') {
            const char *word_end = find_encoded_word_end (cursor);

            if (word_end != NULL) {
                cursor = word_end - 1;
                continue;
            }
        }

        if (escaped) {
            escaped = FALSE;
        } else if (quoted && *cursor == '\\') {
            escaped = TRUE;
        } else if (*cursor == '"') {
            quoted = !quoted;
        } else if (!quoted && *cursor == '>') {
            angle_end = cursor;
            break;
        }
    }
    if (angle_end == NULL) {
        *address_text = g_strdup ("");
        return NULL;
    }

    for (const char *cursor = angle_end + 1; *cursor != '\0'; cursor++) {
        if (!g_ascii_isspace (*cursor)) {
            *address_text = g_strdup ("");
            return NULL;
        }
    }

    *address_text = g_strndup (angle_start + 1, angle_end - angle_start - 1);
    {
        g_autofree char *display_name =
            g_strndup (without_comments, angle_start - without_comments);

        g_strstrip (display_name);
        if (display_name[0] != '\0' &&
            !display_name_phrase_is_valid (display_name)) {
            g_free (*address_text);
            *address_text = g_strdup ("");
            return NULL;
        }
        if (display_name[0] == '"' &&
            display_name[strlen (display_name) - 1] == '"') {
            display_name[strlen (display_name) - 1] = '\0';
            memmove (display_name, display_name + 1, strlen (display_name));
        }

        if (display_name[0] == '\0')
            return NULL;

        return g_steal_pointer (&display_name);
    }
}

static const char *
find_encoded_word_end (const char *start)
{
    const char *charset_end = strchr (start + 2, '?');
    const char *encoding_end = charset_end != NULL ?
        strchr (charset_end + 1, '?') : NULL;

    if (charset_end == NULL || charset_end == start + 2 ||
        encoding_end != charset_end + 2)
        return NULL;

    const char *word_end = strstr (encoding_end + 1, "?=");

    return word_end != NULL ? word_end + 2 : NULL;
}

static void
append_address_segment (GPtrArray *addresses, const char *segment)
{
    g_autofree char *trimmed = g_strdup (segment);
    g_autofree char *address_text = NULL;
    g_autofree char *display_name = NULL;
    g_autofree char *address_without_comments = NULL;
    g_autofree char *normalized_address = NULL;
    gboolean comments_valid = FALSE;

    g_strstrip (trimmed);
    if (*trimmed == '\0')
        return;

    display_name = display_name_from_segment (trimmed, &address_text);
    if (display_name != NULL) {
        g_autofree char *decoded_display_name =
            decode_header_value (display_name);

        g_free (g_steal_pointer (&display_name));
        display_name = g_steal_pointer (&decoded_display_name);
    }
    address_without_comments = remove_comments (address_text,
            &comments_valid);
    if (!comments_valid)
        return;
    normalized_address = normalize_address (address_without_comments);
    if (normalized_address == NULL)
        return;

    ParsedAddress *parsed = g_new0 (ParsedAddress, 1);
    parsed->address = g_steal_pointer (&normalized_address);
    parsed->display_name = g_steal_pointer (&display_name);
    g_ptr_array_add (addresses, parsed);
}

static GPtrArray *
parse_address_list (const char *value)
{
    g_autoptr (GPtrArray) addresses = g_ptr_array_new_with_free_func (
        parsed_address_free);
    g_autoptr (GString) segment = g_string_new (NULL);
    guint comment_depth = 0;
    guint angle_depth = 0;
    guint domain_literal_depth = 0;
    gboolean quoted = FALSE;
    gboolean escaped = FALSE;
    gboolean in_group = FALSE;

    if (value == NULL || *value == '\0')
        return g_steal_pointer (&addresses);

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        char c = *cursor;

        if (c == '=' && cursor[1] == '?') {
            const char *word_end = find_encoded_word_end (cursor);

            if (word_end != NULL) {
                /* Encoded-word punctuation is display-name data. */
                g_string_append_len (segment, cursor, word_end - cursor);
                cursor = word_end - 1;
                continue;
            }
        }

        if (comment_depth > 0) {
            if (escaped) {
                escaped = FALSE;
            } else if (c == '\\') {
                escaped = TRUE;
            } else if (c == '(') {
                comment_depth++;
            } else if (c == ')') {
                comment_depth--;
            }
            g_string_append_c (segment, c);
            continue;
        }

        if (escaped) {
            escaped = FALSE;
        } else if (quoted && c == '\\') {
            escaped = TRUE;
        } else if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && c == '(') {
            comment_depth = 1;
        } else if (!quoted && c == '<') {
            angle_depth++;
        } else if (!quoted && c == '>' && angle_depth > 0) {
            angle_depth--;
        } else if (!quoted && angle_depth == 0 && c == '[') {
            domain_literal_depth++;
        } else if (!quoted && angle_depth == 0 && c == ']' &&
            domain_literal_depth > 0) {
            domain_literal_depth--;
        } else if (!quoted && angle_depth == 0 &&
            domain_literal_depth == 0 && c == ':' && !in_group) {
            g_string_truncate (segment, 0);
            in_group = TRUE;
            continue;
        } else if (!quoted && angle_depth == 0 &&
            domain_literal_depth == 0 && (c == ',' || c == ';')) {
            append_address_segment (addresses, segment->str);
            g_string_truncate (segment, 0);
            if (c == ';')
                in_group = FALSE;
            continue;
        }

        g_string_append_c (segment, c);
    }

    append_address_segment (addresses, segment->str);
    return g_steal_pointer (&addresses);
}

static gboolean
append_parsed_participant_facts (GPtrArray *facts,
    const char *mail_id,
    const GPtrArray *addresses,
    const char *source,
    guint64 created_at_unix_us,
    GError **error)
{
    for (guint i = 0; i < addresses->len; i++) {
        const ParsedAddress *address = g_ptr_array_index (addresses, i);

        if (!append_fact (facts, "participant", mail_id, address->address,
            source, created_at_unix_us, error))
            return FALSE;
        if (address->display_name != NULL &&
            !append_participant_display_name (facts, mail_id,
            address->address, address->display_name, source,
            created_at_unix_us, error))
            return FALSE;
    }

    return TRUE;
}

static gboolean
append_address_facts (GPtrArray *facts,
    const char *mail_id,
    const char *value,
    const char *source,
    guint64 created_at_unix_us,
    GError **error)
{
    g_autoptr (GPtrArray) addresses = parse_address_list (value);

    return append_parsed_participant_facts (facts, mail_id, addresses, source,
               created_at_unix_us, error);
}

static gboolean
append_delivered_to_facts (GPtrArray *facts,
    const char *mail_id,
    const char *value,
    const char *source,
    guint64 created_at_unix_us,
    GError **error)
{
    g_autoptr (GPtrArray) addresses = parse_address_list (value);

    for (guint i = 0; i < addresses->len; i++) {
        const ParsedAddress *address = g_ptr_array_index (addresses, i);

        if (!append_fact (facts, "delivered_to", mail_id, address->address,
            source, created_at_unix_us, error))
            return FALSE;
    }

    return TRUE;
}

static char *
extract_domain_from_normalized_address (const char *address)
{
    const char *at = address != NULL ?
        find_address_separator (address) : NULL;

    if (at == NULL || at[1] == '\0')
        return NULL;

    return g_ascii_strdown (at + 1, -1);
}

static char *
extract_list_id (const char *value)
{
    const char *start = value != NULL ? strchr (value, '<') : NULL;
    const char *end = start != NULL ? strchr (start + 1, '>') : NULL;

    if (start == NULL || end == NULL || end == start + 1)
        return NULL;

    return g_strndup (start + 1, end - start - 1);
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
            message_id = g_strndup (cursor, (gsize)(end - cursor + 1));
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
            G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "dictionary rules are NULL");
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
regex_predicate_is_supported (const char *predicate)
{
    return g_strcmp0 (predicate, "amount_candidate") == 0 ||
           g_strcmp0 (predicate, "date_candidate") == 0 ||
           g_strcmp0 (predicate, "reference_candidate") == 0;
}

static gboolean
compile_regex_rule_pattern (const WyreboxDeterministicFactRegexRule *rule,
    GRegex **out_regex, GError **error)
{
    g_autoptr (GError) regex_error = NULL;
    g_autoptr (GRegex) regex = NULL;
    gint capture_count = 0;

    regex = g_regex_new (rule->pattern, G_REGEX_OPTIMIZE, 0, &regex_error);
    if (regex == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "invalid regex pattern for rule '%s': %s",
            rule->rule_id,
            regex_error != NULL ? regex_error->message : "unknown");
        return FALSE;
    }

    capture_count = g_regex_get_capture_count (regex);
    if (rule->capture_group > (guint)capture_count) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "regex rule '%s' capture group %u is out of range",
            rule->rule_id, rule->capture_group);
        return FALSE;
    }

    *out_regex = g_steal_pointer (&regex);
    return TRUE;
}

static gboolean
validate_regex_rules (const WyreboxDeterministicFactRegexRule *rules,
    gsize n_rules, GError **error)
{
    if (n_rules > 0 && rules == NULL) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "regex rules are NULL");
        return FALSE;
    }

    for (gsize i = 0; i < n_rules; i++) {
        g_autoptr (GRegex) regex = NULL;

        if (rules[i].rule_id == NULL || rules[i].rule_id[0] == '\0') {
            g_set_error (error,
                G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "regex rule id is required");
            return FALSE;
        }

        if (!rule_field_is_supported (rules[i].field)) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "unsupported regex rule field '%s'",
                rules[i].field != NULL ? rules[i].field : "(null)");
            return FALSE;
        }

        if (!regex_predicate_is_supported (rules[i].predicate)) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "unsupported regex rule predicate '%s'",
                rules[i].predicate != NULL ? rules[i].predicate : "(null)");
            return FALSE;
        }

        if (rules[i].pattern == NULL || rules[i].pattern[0] == '\0') {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT, "regex rule pattern is required");
            return FALSE;
        }

        if (!compile_regex_rule_pattern (&rules[i], &regex, error))
            return FALSE;
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
        const char *field_value = get_rule_field_value (metadata,
                rules[i].field);
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

static gboolean
append_regex_rule_facts (GPtrArray *facts,
    const char *mail_id,
    const char *field_value,
    const WyreboxDeterministicFactRegexRule *rule,
    guint64 created_at_unix_us, GError **error)
{
    g_autoptr (GRegex) regex = NULL;
    g_autoptr (GMatchInfo) match_info = NULL;

    if (field_value == NULL || *field_value == '\0')
        return TRUE;

    if (!compile_regex_rule_pattern (rule, &regex, error))
        return FALSE;

    g_regex_match (regex, field_value, 0, &match_info);
    while (g_match_info_matches (match_info)) {
        gint start_pos = -1;
        gint end_pos = -1;
        g_autofree char *value = NULL;
        g_autofree char *source = NULL;

        if (g_match_info_fetch_pos (match_info,
            (gint)rule->capture_group, &start_pos, &end_pos) &&
            start_pos >= 0 && end_pos > start_pos) {
            value = g_match_info_fetch (match_info, (gint)rule->capture_group);
            source = g_strdup_printf ("regex:%s:%s", rule->field,
                    rule->rule_id);
            if (!append_fact (facts,
                rule->predicate, mail_id, value, source, created_at_unix_us,
                error))
                return FALSE;
        }

        {
            g_autoptr (GError) match_error = NULL;

            if (!g_match_info_next (match_info, &match_error)) {
                if (match_error != NULL) {
                    g_propagate_error (error, g_steal_pointer (&match_error));
                    return FALSE;
                }
                break;
            }
        }
    }

    return TRUE;
}

static gboolean
append_regex_facts (GPtrArray *facts,
    const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us,
    const WyreboxDeterministicFactRegexRule *rules,
    gsize n_rules, GError **error)
{
    for (gsize i = 0; i < n_rules; i++) {
        const char *field_value = get_rule_field_value (metadata,
                rules[i].field);

        if (!append_regex_rule_facts (facts,
            mail_id, field_value, &rules[i], created_at_unix_us, error))
            return FALSE;
    }

    return TRUE;
}

gboolean
wyrebox_deterministic_fact_rules_validate (const
    WyreboxDeterministicFactDictionaryRule *dictionary_rules,
    gsize n_dictionary_rules,
    const WyreboxDeterministicFactRegexRule *regex_rules, gsize n_regex_rules,
    GError **error)
{
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    return validate_dictionary_rules (dictionary_rules, n_dictionary_rules,
               error) && validate_regex_rules (regex_rules, n_regex_rules,
               error);
}

GPtrArray *
wyrebox_deterministic_fact_extract_from_metadata_with_rules (const char
    *mail_id, const WyreboxEmlMetadata *metadata, guint64 created_at_unix_us,
    const WyreboxDeterministicFactDictionaryRule *dictionary_rules,
    gsize n_dictionary_rules,
    const WyreboxDeterministicFactRegexRule *regex_rules, gsize n_regex_rules,
    GError **error)
{
    g_autoptr (GPtrArray) facts = NULL;
    g_autoptr (GPtrArray) sender_addresses = NULL;
    g_autofree char *decoded_subject = NULL;
    g_autofree char *decoded_from = NULL;
    g_autofree char *decoded_to = NULL;
    g_autofree char *decoded_cc = NULL;
    g_autofree char *decoded_bcc = NULL;
    g_autofree char *sender_domain = NULL;
    g_autofree char *list_id = NULL;
    WyreboxEmlMetadata working_metadata = { 0 };
    const char *delivered_to = NULL;
    const char *delivered_to_source = NULL;

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
            G_IO_ERROR_INVALID_ARGUMENT,
            "fact extraction timestamp is required");
        return NULL;
    }

    if (!wyrebox_deterministic_fact_rules_validate (dictionary_rules,
        n_dictionary_rules, regex_rules, n_regex_rules, error))
        return NULL;

    decoded_subject = decode_header_value (metadata->subject);
    decoded_from = decode_header_value (metadata->from);
    decoded_to = decode_header_value (metadata->to);
    decoded_cc = decode_header_value (metadata->cc);
    decoded_bcc = decode_header_value (metadata->bcc);
    working_metadata = *metadata;
    working_metadata.subject = decoded_subject;
    working_metadata.from = decoded_from;
    working_metadata.to = decoded_to;
    working_metadata.cc = decoded_cc;
    working_metadata.bcc = decoded_bcc;

    facts = g_ptr_array_new_with_free_func (fact_record_free);

    if (metadata->message_id != NULL && metadata->message_id[0] != '\0') {
        if (!append_fact (facts,
            "message_id",
            mail_id,
            metadata->message_id,
            "header:message-id", created_at_unix_us, error))
            return NULL;
    }

    sender_addresses = parse_address_list (metadata->from);
    if (sender_addresses->len > 0) {
        const ParsedAddress *sender =
            g_ptr_array_index (sender_addresses, 0);

        sender_domain =
            extract_domain_from_normalized_address (sender->address);
    }
    if (sender_domain != NULL) {
        if (!append_fact (facts,
            "sender_domain",
            mail_id, sender_domain, "header:from", created_at_unix_us, error))
            return NULL;
    }

    if (!append_parsed_participant_facts (facts, mail_id, sender_addresses,
        "header:from", created_at_unix_us, error))
        return NULL;

    if (!append_address_facts (facts,
        mail_id, metadata->to, "header:to", created_at_unix_us, error))
        return NULL;

    if (!append_address_facts (facts,
        mail_id, metadata->cc, "header:cc", created_at_unix_us, error))
        return NULL;

    if (!append_address_facts (facts,
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

    list_id = extract_list_id (metadata->list_id);
    if (list_id != NULL &&
        !append_fact (facts,
        "list_id", mail_id, list_id, "header:list-id",
        created_at_unix_us, error))
        return NULL;

    if (metadata->delivered_to != NULL) {
        delivered_to = metadata->delivered_to;
        delivered_to_source = "header:delivered-to";
    } else {
        delivered_to = metadata->x_original_to;
        delivered_to_source = "header:x-original-to";
    }
    if (!append_delivered_to_facts (facts, mail_id, delivered_to,
        delivered_to_source, created_at_unix_us, error))
        return NULL;

    if (!append_dictionary_facts (facts,
        mail_id, &working_metadata, created_at_unix_us, dictionary_rules,
        n_dictionary_rules, error))
        return NULL;

    if (!append_regex_facts (facts,
        mail_id, &working_metadata, created_at_unix_us, regex_rules,
        n_regex_rules,
        error))
        return NULL;

    return g_steal_pointer (&facts);
}

GPtrArray *
wyrebox_deterministic_fact_extract_from_metadata_with_dictionary (const char
    *mail_id, const WyreboxEmlMetadata *metadata, guint64 created_at_unix_us,
    const WyreboxDeterministicFactDictionaryRule *rules, gsize n_rules,
    GError **error)
{
    return wyrebox_deterministic_fact_extract_from_metadata_with_rules (mail_id,
               metadata, created_at_unix_us, rules, n_rules, NULL, 0, error);
}

GPtrArray *
wyrebox_deterministic_fact_extract_from_metadata_with_regex (const char
    *mail_id, const WyreboxEmlMetadata *metadata, guint64 created_at_unix_us,
    const WyreboxDeterministicFactRegexRule *rules, gsize n_rules,
    GError **error)
{
    return wyrebox_deterministic_fact_extract_from_metadata_with_rules (mail_id,
               metadata, created_at_unix_us, NULL, 0, rules, n_rules, error);
}

GPtrArray *
wyrebox_deterministic_fact_extract_from_metadata (const char *mail_id,
    const WyreboxEmlMetadata *metadata,
    guint64 created_at_unix_us, GError **error)
{
    return
        wyrebox_deterministic_fact_extract_from_metadata_with_rules (mail_id,
            metadata, created_at_unix_us, NULL, 0, NULL, 0, error);
}
