#include "wyrebox-benchmark-report.h"

#include <sys/resource.h>
#include <sys/time.h>

void
wyrebox_benchmark_report_clear (WyreboxBenchmarkReport *report)
{
  if (report == NULL)
    return;

  memset (report, 0, sizeof (*report));
}

void
wyrebox_benchmark_report_init (WyreboxBenchmarkReport *report)
{
  g_return_if_fail (report != NULL);

  wyrebox_benchmark_report_clear (report);
}

void
wyrebox_benchmark_report_capture_rusage (WyreboxBenchmarkReport *report)
{
  struct rusage usage = { 0 };

  g_return_if_fail (report != NULL);

  if (getrusage (RUSAGE_SELF, &usage) != 0)
    return;

  report->cpu_user_us = (gint64) usage.ru_utime.tv_sec * G_USEC_PER_SEC
      + (gint64) usage.ru_utime.tv_usec;
  report->cpu_system_us = (gint64) usage.ru_stime.tv_sec * G_USEC_PER_SEC
      + (gint64) usage.ru_stime.tv_usec;
  report->max_rss_kb = (gsize) usage.ru_maxrss;
}

void
wyrebox_benchmark_report_print_json (const char *suite,
    const char *case_name, const WyreboxBenchmarkReport *report)
{
  g_return_if_fail (suite != NULL);
  g_return_if_fail (case_name != NULL);
  g_return_if_fail (report != NULL);

  g_print ("{\"schema\":\"wyrebox-benchmark-report/v1\",");
  g_print ("\"suite\":\"%s\",", suite);
  g_print ("\"case\":\"%s\",", case_name);
  g_print ("\"metric\":\"elapsed_us\",");
  g_print ("\"value\":%" G_GINT64_FORMAT ",", report->elapsed_us);
  g_print ("\"cpu_user_us\":%" G_GINT64_FORMAT ",", report->cpu_user_us);
  g_print ("\"cpu_system_us\":%" G_GINT64_FORMAT ",", report->cpu_system_us);
  g_print ("\"max_rss_kb\":%" G_GSIZE_FORMAT ",", report->max_rss_kb);
  g_print ("\"object_count\":%" G_GUINT64_FORMAT ",", report->object_count);
  g_print ("\"disk_bytes\":%" G_GUINT64_FORMAT ",", report->disk_bytes);
  g_print ("\"status\":\"ok\"}\n");
}
