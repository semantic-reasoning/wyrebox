#pragma once

#include "wyrebox-wirelog-evaluation.h"

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  char *view_id;
  char *message_id;
} WyreboxWirelogDerivedMembership;

void wyrebox_wirelog_derived_membership_clear (
    WyreboxWirelogDerivedMembership *membership);

void wyrebox_wirelog_derived_membership_free (
    WyreboxWirelogDerivedMembership *membership);

GPtrArray *wyrebox_wirelog_derived_membership_parse_csv (
    const char *csv,
    GError **error);

GPtrArray *wyrebox_wirelog_derived_membership_parse_evaluation_relation (
    WyreboxWirelogEvaluation *evaluation,
    const char *relation_name,
    GError **error);

GPtrArray *wyrebox_wirelog_derived_membership_snapshot_from_rules_and_facts (
    const char *rules_source,
    GPtrArray *facts,
    const char *relation_name,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxWirelogDerivedMembership,
    wyrebox_wirelog_derived_membership_clear)

G_END_DECLS
/* *INDENT-ON* */
