#ifndef MAIL_STORAGE_PRIVATE_H
#define MAIL_STORAGE_PRIVATE_H

#include "mail-storage.h"
#include "mail-storage-hooks.h"

struct file_lock;

struct mailbox;
struct mailbox_list;
struct mail_storage;
struct mail;
struct mailbox_transaction_context;
struct mail_search_context;
struct mail_search_args;
struct mailbox_header_lookup_ctx;
struct message_part;
struct mail_keywords;
struct message_size;
struct istream;
struct mail_save_context;
struct mail_index_transaction_commit_result;
struct mailbox_update;
struct mailbox_status;
struct mailbox_vfuncs;
struct mail_vfuncs;

struct mail_storage_vfuncs {
  void (*add_list)(struct mail_storage *storage,
                   struct mailbox_list *list);
  struct mailbox *(*mailbox_alloc)(struct mail_storage *storage,
                                  struct mailbox_list *list,
                                  const char *vname);
};

struct mailbox_vfuncs {
  int (*open)(struct mailbox *box);
  int (*get_status)(struct mailbox *box, int items, struct mailbox_status *status_r);
  struct mail_search_context *(*search_init)(
      struct mailbox_transaction_context *transaction,
      struct mail_search_args *args,
      const int *sort,
      int fields,
      struct mailbox_header_lookup_ctx *headers);
};

struct mail_vfuncs {
  int (*get_stream)(struct mail *mail, int get_body,
                    struct message_size *hdr_size,
                    struct message_size *body_size,
                    struct istream **stream);
  void (*update_flags)(struct mail *mail, int modify_type, int flags);
  void (*update_keywords)(struct mail *mail, int modify_type,
                         struct mail_keywords *keywords);
};

struct virtual_mailbox_vfuncs {
  void (*get_virtual_uids)(struct mailbox *box,
                          struct mailbox *backend_mailbox,
                          const void *backend_uids,
                          void *virtual_uids_r);
};

struct mailbox {
  struct mailbox_vfuncs v;
};

struct mail {
  const struct mail_vfuncs *v;
};

struct mail_storage {
  struct mail_storage_vfuncs v;
};

#endif
