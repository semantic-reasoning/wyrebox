#ifndef MAILBOX_LIST_PRIVATE_H
#define MAILBOX_LIST_PRIVATE_H

#include <stdbool.h>

#include "lib.h"
#include "mail-storage.h"
#include "mailbox-list.h"
#include "mailbox-list-iter.h"

struct imap_match_glob;
struct mailbox_list_autocreate_iterate_context;

struct mailbox_list_vfuncs
{
  struct mailbox_list *(*alloc) (void);
  int (*init) (struct mailbox_list * list, const char **error_r);
  void (*deinit) (struct mailbox_list * list);

  struct mailbox_list_iterate_context *(*scan_init) (struct mailbox_list *
      list, const char *const *patterns, enum mailbox_list_iter_flags flags);
  const struct mailbox_info *(*scan_next) (struct
      mailbox_list_iterate_context * ctx);
  int (*scan_deinit) (struct mailbox_list_iterate_context * ctx);
};

struct mailbox_list
{
  struct mailbox_list_vfuncs v, *vlast;
};

struct mailbox_list_iterate_context
{
  struct mailbox_list *list;
  pool_t pool;
  enum mailbox_list_iter_flags flags;
  bool failed;
  bool index_iteration;

  struct imap_match_glob *glob;
  struct mailbox_list_autocreate_iterate_context *autocreate_ctx;
  struct mailbox_info specialuse_info;
};

#endif
