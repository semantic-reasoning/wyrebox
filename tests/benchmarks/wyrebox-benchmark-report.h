#pragma once

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  gint64 elapsed_us;
  gint64 cpu_user_us;
  gint64 cpu_system_us;
  gsize max_rss_kb;
  guint64 object_count;
  guint64 disk_bytes;
} WyreboxBenchmarkReport;

void wyrebox_benchmark_report_clear (WyreboxBenchmarkReport *report);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxBenchmarkReport,
    wyrebox_benchmark_report_clear)

void wyrebox_benchmark_report_init (WyreboxBenchmarkReport *report);

void wyrebox_benchmark_report_capture_rusage (WyreboxBenchmarkReport *report);

void wyrebox_benchmark_report_print_json (const char *suite,
    const char *case_name,
    const WyreboxBenchmarkReport *report);

G_END_DECLS
/* *INDENT-ON* */
