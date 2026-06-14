#ifndef MAIL_STORAGE_PRIVATE_H
#define MAIL_STORAGE_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

#include "mail-storage.h"
#include "mail-storage-hooks.h"
#include "mail-namespace.h"

struct file_lock;

struct mailbox;
struct mailbox_list;
struct array;
struct mail_namespace;
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
struct mailbox_sync_context;
struct mailbox_sync_rec;
struct mailbox_sync_status;
struct event;
struct mailbox_vfuncs;
struct mail_vfuncs;

enum mailbox_feature
{
  MAILBOX_FEATURE_CONDSTORE = 0x01,
};

enum mailbox_sync_flags
{
  MAILBOX_SYNC_FLAG_FULL_READ = 0x01,
  MAILBOX_SYNC_FLAG_FULL_WRITE = 0x02,
  MAILBOX_SYNC_FLAG_FAST = 0x04,
};

enum mailbox_sync_type
{
  MAILBOX_SYNC_TYPE_EXPUNGE = 0x01,
  MAILBOX_SYNC_TYPE_FLAGS = 0x02,
  MAILBOX_SYNC_TYPE_MODSEQ = 0x04,
};

enum mailbox_list_child_state
{
  MAILBOX_LIST_CHILD_STATE_UNKNOWN = 0,
  MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN,
  MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN,
};

struct mailbox_list_sink_entry
{
  const char *name;
  char hierarchy_delimiter;
  bool selectable;
  enum mailbox_list_child_state child_state;
  const char *special_use;
};

struct mailbox_sync_rec
{
  uint32_t seq1;
  uint32_t seq2;
  enum mailbox_sync_type type;
};

struct mailbox_sync_status
{
  bool sync_delayed_expunges;
};

struct mailbox_sync_context
{
  struct mailbox *box;
  enum mailbox_sync_flags flags;
  bool open_failed;
};

const char *mailbox_list_get_storage_name (struct mailbox_list *list,
    const char *vname);
struct mailbox_list *mailbox_list_sink_alloc (void);
void mailbox_list_sink_free (struct mailbox_list *list);
void mailbox_list_sink_fail_next_publish (struct mailbox_list *list);
bool mailbox_list_sink_publish_entry (struct mailbox_list *list,
    const char *name,
    char hierarchy_delimiter,
    bool selectable,
    enum mailbox_list_child_state child_state, const char *special_use);
unsigned int mailbox_list_sink_get_count (const struct mailbox_list *list);
const struct mailbox_list_sink_entry *mailbox_list_sink_get_entry (const struct
    mailbox_list *list, unsigned int index);
unsigned int mailbox_list_sink_get_original_iter_init_calls (const struct
    mailbox_list *list);
unsigned int mailbox_list_sink_get_original_iter_next_calls (const struct
    mailbox_list *list);
unsigned int mailbox_list_sink_get_original_iter_deinit_calls (const struct
    mailbox_list *list);
unsigned int mailbox_list_sink_get_original_deinit_calls (const struct
    mailbox_list *list);
void mail_storage_set_error (struct mail_storage *storage,
    enum mail_error error, const char *string);

struct mail_storage_vfuncs
{
  struct mail_storage *(*alloc) (void);
  int (*create) (struct mail_storage * storage,
      struct mail_namespace * ns, const char **error_r);
  void (*destroy) (struct mail_storage * storage);
  void (*add_list) (struct mail_storage * storage, struct mailbox_list * list);
  struct mailbox *(*mailbox_alloc) (struct mail_storage * storage,
      struct mailbox_list * list, const char *vname, enum mailbox_flags flags);
};

struct mailbox_vfuncs
{
  bool (*is_readonly) (struct mailbox * box);
  int (*enable) (struct mailbox * box, enum mailbox_feature features);
  int (*open) (struct mailbox * box);
  int (*get_status) (struct mailbox * box,
      enum mailbox_status_items items, struct mailbox_status * status_r);
  void (*free) (struct mailbox * box);
  void (*close) (struct mailbox * box);
  struct mailbox_sync_context *(*sync_init) (struct mailbox * box,
      enum mailbox_sync_flags flags);
  bool (*sync_next) (struct mailbox_sync_context * ctx,
      struct mailbox_sync_rec * sync_rec_r);
  int (*sync_deinit) (struct mailbox_sync_context * ctx,
      struct mailbox_sync_status * status_r);
  struct mail_search_context *(*search_init) (struct mailbox_transaction_context
      * transaction, struct mail_search_args * args, const int *sort,
      int fields, struct mailbox_header_lookup_ctx * headers);
};

struct mail_vfuncs
{
  int (*get_stream) (struct mail * mail, int get_body,
      struct message_size * hdr_size,
      struct message_size * body_size, struct istream ** stream);
  void (*update_flags) (struct mail * mail, int modify_type, int flags);
  void (*update_keywords) (struct mail * mail, int modify_type,
      struct mail_keywords * keywords);
};

struct virtual_mailbox_vfuncs
{
  void (*get_virtual_uids) (struct mailbox * box,
      struct mailbox * backend_mailbox,
      const void *backend_uids, void *virtual_uids_r);
};

struct mail_storage
{
  const char *name;
  unsigned int class_flags;
  struct event *event;
  pool_t pool;
  struct mail_storage_vfuncs v;
};

struct mailbox
{
  const char *name;
  const char *vname;
  struct mail_storage *storage;
  struct mailbox_list *list;
  struct event *event;

  struct mailbox_vfuncs v;
  struct mailbox_vfuncs *vlast;
  const struct mail_vfuncs *mail_vfuncs;
  array_t search_results;
  array_t module_contexts;
  bool opened;
  enum mailbox_feature enabled_features;

  pool_t pool;
};

struct istream
{
  void *data;
  unsigned int size;
};

struct mail
{
  struct mailbox *box;
  unsigned int uid;
  const struct mail_vfuncs *v;
};

void i_stream_unref (struct istream **stream);

#endif
