#pragma once

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  char *definition_ref;
  char *rules_source;
  char *rule_version_hash;
  char *relation_name;
} WyreboxWirelogRuleDefinition;

void wyrebox_wirelog_rule_definition_clear (
    WyreboxWirelogRuleDefinition *definition);

void wyrebox_wirelog_rule_definition_free (
    WyreboxWirelogRuleDefinition *definition);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxWirelogRuleDefinition,
    wyrebox_wirelog_rule_definition_clear)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (WyreboxWirelogRuleDefinition,
    wyrebox_wirelog_rule_definition_free)

WyreboxWirelogRuleDefinition *wyrebox_wirelog_rule_definition_load_file (
    GFile *file,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
