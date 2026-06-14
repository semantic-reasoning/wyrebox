#include "wyrebox-rfc5322-date.h"

#include <errno.h>
#include <string.h>

static gboolean
parse_uint_token (const gchar *token, guint min_len, guint max_len,
    gint64 *out_value)
{
  gchar *end = NULL;
  guint len = 0;
  gint64 value = 0;

  if (token == NULL || *token == '\0')
    return FALSE;

  len = strlen (token);
  if (len < min_len || len > max_len)
    return FALSE;

  for (guint i = 0; i < len; i++) {
    if (!g_ascii_isdigit (token[i]))
      return FALSE;
  }

  errno = 0;
  value = g_ascii_strtoll (token, &end, 10);
  if (errno != 0 || end == NULL || *end != '\0')
    return FALSE;

  *out_value = value;
  return TRUE;
}

static gboolean
parse_month (const gchar *token, gint *out_month)
{
  static const gchar *months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };

  if (token == NULL)
    return FALSE;

  for (guint i = 0; i < G_N_ELEMENTS (months); i++) {
    if (g_ascii_strcasecmp (token, months[i]) == 0) {
      *out_month = (gint) i + 1;
      return TRUE;
    }
  }

  return FALSE;
}

static gboolean
parse_weekday (const gchar *token, gint *out_weekday)
{
  static const struct
  {
    const gchar *abbrev;
    const gchar *name;
    gint weekday;
  } weekdays[] = {
    {"Mon", "Monday", G_DATE_MONDAY},
    {"Tue", "Tuesday", G_DATE_TUESDAY},
    {"Wed", "Wednesday", G_DATE_WEDNESDAY},
    {"Thu", "Thursday", G_DATE_THURSDAY},
    {"Fri", "Friday", G_DATE_FRIDAY},
    {"Sat", "Saturday", G_DATE_SATURDAY},
    {"Sun", "Sunday", G_DATE_SUNDAY},
  };
  g_autofree gchar *weekday = NULL;
  gsize len = 0;

  if (token == NULL || !g_str_has_suffix (token, ","))
    return FALSE;

  len = strlen (token);
  if (len <= 1)
    return FALSE;

  weekday = g_strndup (token, len - 1);
  for (guint i = 0; i < G_N_ELEMENTS (weekdays); i++) {
    if (g_ascii_strcasecmp (weekday, weekdays[i].abbrev) == 0 ||
        g_ascii_strcasecmp (weekday, weekdays[i].name) == 0) {
      *out_weekday = weekdays[i].weekday;
      return TRUE;
    }
  }

  return FALSE;
}

static gboolean
parse_year (const gchar *token, gint *out_year)
{
  gint64 year = 0;
  guint len = 0;

  if (token == NULL)
    return FALSE;

  len = strlen (token);
  if (len < 2 || len > 4)
    return FALSE;

  if (!parse_uint_token (token, len, len, &year))
    return FALSE;

  if (len == 2)
    year += year >= 50 ? 1900 : 2000;
  else if (len == 3)
    year += 1900;
  else if (year < 1900)
    return FALSE;

  *out_year = (gint) year;
  return TRUE;
}

static gboolean
parse_time (const gchar *token, gint *out_hour, gint *out_minute,
    gint *out_second)
{
  g_auto (GStrv) parts = NULL;
  gint64 hour = 0;
  gint64 minute = 0;
  gint64 second = 0;
  guint count = 0;

  if (token == NULL)
    return FALSE;

  parts = g_strsplit (token, ":", 0);
  while (parts[count] != NULL)
    count++;

  if (count != 2 && count != 3)
    return FALSE;

  if (!parse_uint_token (parts[0], 2, 2, &hour) ||
      !parse_uint_token (parts[1], 2, 2, &minute))
    return FALSE;

  if (count == 3 && !parse_uint_token (parts[2], 2, 2, &second))
    return FALSE;

  if (hour > 23 || minute > 59 || second > 59)
    return FALSE;

  *out_hour = (gint) hour;
  *out_minute = (gint) minute;
  *out_second = (gint) second;
  return TRUE;
}

static gboolean
parse_timezone_offset_seconds (const gchar *token, gint *out_offset_seconds)
{
  gint sign = 1;
  gint64 hours = 0;
  gint64 minutes = 0;
  gchar hour_text[3] = { 0 };
  gchar minute_text[3] = { 0 };

  if (token == NULL)
    return FALSE;

  if (g_ascii_strcasecmp (token, "GMT") == 0 ||
      g_ascii_strcasecmp (token, "UTC") == 0 ||
      g_ascii_strcasecmp (token, "UT") == 0) {
    *out_offset_seconds = 0;
    return TRUE;
  }

  if (strlen (token) != 5 || (token[0] != '+' && token[0] != '-'))
    return FALSE;

  sign = token[0] == '-' ? -1 : 1;
  memcpy (hour_text, token + 1, 2);
  memcpy (minute_text, token + 3, 2);

  if (!parse_uint_token (hour_text, 2, 2, &hours) ||
      !parse_uint_token (minute_text, 2, 2, &minutes))
    return FALSE;

  if (hours > 23 || minutes > 59)
    return FALSE;

  *out_offset_seconds = sign * (gint) ((hours * 60 + minutes) * 60);
  return TRUE;
}

static gboolean
skip_weekday_token (GStrv tokens, guint *out_start, gboolean *out_has_weekday,
    gint *out_weekday)
{
  if (tokens[0] == NULL || tokens[1] == NULL)
    return TRUE;

  if (!g_str_has_suffix (tokens[0], ","))
    return TRUE;

  if (!parse_weekday (tokens[0], out_weekday))
    return FALSE;

  *out_start = 1;
  *out_has_weekday = TRUE;
  return TRUE;
}

gboolean
wyrebox_rfc5322_date_parse_unix_us (const gchar *value, gint64 *out_unix_us)
{
  g_auto (GStrv) tokens = NULL;
  g_autoptr (GDateTime) local = NULL;
  g_autoptr (GDateTime) utc = NULL;
  g_autofree gchar *stripped = NULL;
  gint64 day = 0;
  gint year = 0;
  gint month = 0;
  gint hour = 0;
  gint minute = 0;
  gint second = 0;
  gint offset_seconds = 0;
  gint weekday = 0;
  guint start = 0;
  guint count = 0;
  gboolean has_weekday = FALSE;

  if (out_unix_us != NULL)
    *out_unix_us = 0;

  if (value == NULL)
    return FALSE;

  stripped = g_strstrip (g_strdup (value));
  if (*stripped == '\0')
    return FALSE;

  tokens = g_strsplit_set (stripped, " \t", 0);
  for (guint read = 0, write = 0; tokens[read] != NULL; read++) {
    if (tokens[read][0] == '\0') {
      g_free (tokens[read]);
      continue;
    }

    if (read != write)
      tokens[write] = tokens[read];
    write++;
    count = write;
  }
  tokens[count] = NULL;

  if (!skip_weekday_token (tokens, &start, &has_weekday, &weekday))
    return FALSE;

  if (count - start != 5)
    return FALSE;

  if (!parse_uint_token (tokens[start], 1, 2, &day) ||
      !parse_month (tokens[start + 1], &month) ||
      !parse_year (tokens[start + 2], &year) ||
      !parse_time (tokens[start + 3], &hour, &minute, &second) ||
      !parse_timezone_offset_seconds (tokens[start + 4], &offset_seconds))
    return FALSE;

  local = g_date_time_new_utc (year, month, (gint) day, hour, minute,
      (gdouble) second);
  if (local == NULL)
    return FALSE;

  if (has_weekday && weekday != g_date_time_get_day_of_week (local))
    return FALSE;

  utc = g_date_time_add_seconds (local, -offset_seconds);
  if (utc == NULL)
    return FALSE;

  if (out_unix_us != NULL)
    *out_unix_us = g_date_time_to_unix (utc) * G_GINT64_CONSTANT (1000000) +
        g_date_time_get_microsecond (utc);

  return TRUE;
}
