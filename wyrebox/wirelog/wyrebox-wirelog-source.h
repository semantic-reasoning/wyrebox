#pragma once

#include "wyrebox-fact-record.h"
#include "wyrebox-wirelog-program.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

char *wyrebox_wirelog_source_build (
    const char *rules_source,
    GPtrArray *facts,
    GError **error);

WyreboxWirelogProgram *wyrebox_wirelog_program_new_from_rules_and_facts (
    const char *rules_source,
    GPtrArray *facts,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
