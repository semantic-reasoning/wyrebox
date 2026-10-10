#ifndef MAILBOX_LIST_ITER_H
#define MAILBOX_LIST_ITER_H

#include "mail-namespace.h"
#include "mailbox-list.h"

enum mailbox_list_iter_flags
{
  MAILBOX_LIST_ITER_RAW_LIST = 0x000001,
  MAILBOX_LIST_ITER_NO_AUTO_BOXES = 0x000004,
  MAILBOX_LIST_ITER_SKIP_ALIASES = 0x000008,
  MAILBOX_LIST_ITER_STAR_WITHIN_NS = 0x000010,

  MAILBOX_LIST_ITER_SELECT_SUBSCRIBED = 0x000100,
  MAILBOX_LIST_ITER_SELECT_RECURSIVEMATCH = 0x000200,
  MAILBOX_LIST_ITER_SELECT_SPECIALUSE = 0x000400,

  MAILBOX_LIST_ITER_RETURN_NO_FLAGS = 0x001000,
  MAILBOX_LIST_ITER_RETURN_SUBSCRIBED = 0x002000,
  MAILBOX_LIST_ITER_RETURN_CHILDREN = 0x004000,
  MAILBOX_LIST_ITER_RETURN_SPECIALUSE = 0x008000,
};

struct mailbox_info
{
  const char *vname;
  const char *special_use;
  enum mailbox_info_flags flags;

  struct mail_namespace *ns;
};

struct mailbox_list_iterate_context *mailbox_list_iter_init (struct mailbox_list
    *list, const char *pattern, enum mailbox_list_iter_flags flags);
const struct mailbox_info *mailbox_list_iter_next (struct
    mailbox_list_iterate_context *ctx);
int mailbox_list_iter_deinit (struct mailbox_list_iterate_context **ctx);

#endif
