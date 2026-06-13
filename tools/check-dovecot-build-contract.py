#!/usr/bin/env python3

"""Validate a configured Dovecot build directory for WyreBox build-header checks."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import argparse
import os
import shlex
import shutil
import subprocess
import tempfile
import re
import sys
import textwrap


REQUIRED_CONFIG_MACROS = (
    "DOVECOT_ABI_VERSION",
    "HAVE__BOOL",
    "HAVE_SOCKLEN_T",
    "OFF_T_MAX",
    "PRIuUOFF_T",
    "SIZEOF_INT",
    "SIZEOF_LONG",
    "SIZEOF_VOID_P",
    "SSIZE_T_MAX",
    "UOFF_T_MAX",
)

PINNED_DOVECOT_ABI_VERSION = "2.3.ABIv21(2.3.21.1)"

UOFF_T_SELECTOR_MACROS = (
    "HAVE_UOFF_T",
    "UOFF_T_INT",
    "UOFF_T_LONG",
    "UOFF_T_LONG_LONG",
)


MAILBOX_VFUNC_PROBE_SOURCE = textwrap.dedent(
    """\
    #include <stddef.h>
    #include "config.h"
    #include "lib.h"
    #include "mail-storage.h"
    #include "mail-storage-private.h"

    typedef struct mailbox *(*wyrebox_mailbox_alloc_fn)(
        struct mail_storage *storage,
        struct mailbox_list *list,
        const char *vname,
        enum mailbox_flags flags);

    typedef struct mail_storage *(*wyrebox_mail_storage_alloc_fn)(void);

    typedef int (*wyrebox_mail_storage_create_fn)(
        struct mail_storage *storage,
        struct mail_namespace *ns,
        const char **error_r);

    typedef void (*wyrebox_mail_storage_destroy_fn)(
        struct mail_storage *storage);

    typedef int (*wyrebox_mailbox_open_fn)(struct mailbox *box);
    typedef void (*wyrebox_mailbox_free_fn)(struct mailbox *box);

    typedef int (*wyrebox_mailbox_get_status_fn)(
        struct mailbox *box,
        enum mailbox_status_items items,
        struct mailbox_status *status_r);

    static struct mail_storage wyrebox_mail_storage_probe;
    static struct mailbox wyrebox_mailbox_probe;

    static struct mailbox *wyrebox_probe_mailbox_alloc(
        struct mail_storage *storage,
        struct mailbox_list *list,
        const char *vname,
        enum mailbox_flags flags);

    static struct mail_storage *wyrebox_probe_mail_storage_alloc(void);

    static int wyrebox_probe_mail_storage_create(
        struct mail_storage *storage,
        struct mail_namespace *ns,
        const char **error_r);

    static void wyrebox_probe_mail_storage_destroy(struct mail_storage *storage);

    static int wyrebox_probe_mailbox_open(struct mailbox *box);
    static void wyrebox_probe_mailbox_free(struct mailbox *box);

    static int wyrebox_probe_mailbox_get_status(
        struct mailbox *box,
        enum mailbox_status_items items,
        struct mailbox_status *status_r);

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mail_storage_probe.v.mailbox_alloc),
            wyrebox_mailbox_alloc_fn),
        "mail_storage_vfuncs::mailbox_alloc signature mismatch");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mail_storage_probe.v.alloc),
            wyrebox_mail_storage_alloc_fn),
        "mail_storage_vfuncs::alloc signature mismatch");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mail_storage_probe.v.create),
            wyrebox_mail_storage_create_fn),
        "mail_storage_vfuncs::create signature mismatch");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mail_storage_probe.v.destroy),
            wyrebox_mail_storage_destroy_fn),
        "mail_storage_vfuncs::destroy signature mismatch");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.v.open),
            wyrebox_mailbox_open_fn),
        "mailbox_vfuncs::open signature mismatch");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.v.free),
            wyrebox_mailbox_free_fn),
        "mailbox_vfuncs::free signature mismatch");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.name),
            const char *),
        "mailbox struct::name expected const char pointer");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.vname),
            const char *),
        "mailbox struct::vname expected const char pointer");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.storage),
            struct mail_storage *),
        "mailbox struct::storage expected struct mail_storage pointer");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.list),
            struct mailbox_list *),
        "mailbox struct::list expected struct mailbox_list pointer");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.vlast),
            struct mailbox_vfuncs *),
        "mailbox struct::vlast expected struct mailbox_vfuncs pointer");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.v.get_status),
            wyrebox_mailbox_get_status_fn),
        "mailbox_vfuncs::get_status signature mismatch");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.mail_vfuncs),
            const struct mail_vfuncs *),
        "mailbox::mail_vfuncs type mismatch");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.pool),
            pool_t),
        "mailbox struct::pool expected pool_t");

    _Static_assert(
        __builtin_types_compatible_p(
            __typeof__(wyrebox_mailbox_probe.event),
            struct event *),
        "mailbox struct::event expected struct event pointer");

    _Static_assert(
        __builtin_types_compatible_p(
        __typeof__(((struct mailbox *)0)->v.open),
        wyrebox_mailbox_open_fn),
        "mailbox field .v.open signature mismatch");

    static struct mail_storage wyrebox_mail_storage_probe_template = {
        .name = "wyrebox",
        .class_flags = 0,
        .v = {
              .alloc = wyrebox_probe_mail_storage_alloc,
              .create = wyrebox_probe_mail_storage_create,
              .destroy = wyrebox_probe_mail_storage_destroy,
              .add_list = NULL,
              .mailbox_alloc = NULL,
            },
    };

    static struct mailbox *wyrebox_probe_mailbox_alloc(
        struct mail_storage *storage,
        struct mailbox_list *list,
        const char *vname,
        enum mailbox_flags flags) {
      struct mailbox *box;
      pool_t pool;

      pool = pool_alloconly_create("wyrebox mailbox probe", 1024);
      box = p_new(pool, struct mailbox, 1);
      box->pool = pool;
      box->storage = storage;
      box->list = list;
      box->vname = vname;
      box->name = vname;
      box->mail_vfuncs = NULL;

      box->vlast = NULL;
      box->v.open = wyrebox_probe_mailbox_open;
      box->v.free = wyrebox_probe_mailbox_free;
      box->v.get_status = wyrebox_probe_mailbox_get_status;
      (void)flags;
      return box;
    }

    static struct mail_storage wyrebox_mail_storage_probe_storage = {
        .name = "wyrebox",
    };

    static struct mail_storage *
    wyrebox_probe_mail_storage_alloc(void) {
      return &wyrebox_mail_storage_probe_storage;
    }

    static int
    wyrebox_probe_mail_storage_create(
        struct mail_storage *storage,
        struct mail_namespace *ns,
        const char **error_r) {
      (void) storage;
      (void) ns;
      (void) error_r;
      return 0;
    }

    static void
    wyrebox_probe_mail_storage_destroy(struct mail_storage *storage) {
      (void) storage;
    }

    static int wyrebox_probe_mailbox_open(struct mailbox *box) {
      (void)box;
      return 0;
    }

    static void wyrebox_probe_mailbox_free(struct mailbox *box) {
      (void)box;
    }

    static int wyrebox_probe_mailbox_get_status(
        struct mailbox *box,
        enum mailbox_status_items items,
        struct mailbox_status *status_r) {
      (void)box;
      (void)items;
      (void)status_r;
      return 0;
    }

    int
    main(void)
    {
      wyrebox_mail_storage_probe.v = wyrebox_mail_storage_probe_template.v;
      wyrebox_mail_storage_probe_template.v = wyrebox_mail_storage_probe.v;
      wyrebox_mailbox_probe.v.open = wyrebox_probe_mailbox_open;
      wyrebox_mailbox_probe.v.free = wyrebox_probe_mailbox_free;
      wyrebox_mailbox_probe.v.get_status = wyrebox_probe_mailbox_get_status;
      (void)wyrebox_mail_storage_probe;
      (void)wyrebox_mailbox_probe;
      return 0;
    }
    """
)


@dataclass
class ContractIssue:
    message: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate that a configured Dovecot build directory has "
            "the headers and macros WyreBox's Dovecot module compile checks need"
        ),
    )
    parser.add_argument("source_dir")
    parser.add_argument("build_dir")
    return parser.parse_args()


def resolve_dir(raw_dir: str, label: str) -> Path:
    directory = Path(raw_dir)
    if not directory.exists():
        raise FileNotFoundError(f"{label} directory not found: {directory}")
    if not directory.is_dir():
        raise NotADirectoryError(f"{label} path is not a directory: {directory}")
    return directory


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def find_issues_for_required_macros(config_h: str, required_macros: tuple[str, ...]) -> list[ContractIssue]:
    issues = []
    for macro in required_macros:
        if re.search(rf"^\s*#\s*define\s+{re.escape(macro)}\b", config_h, re.MULTILINE) is None:
            issues.append(ContractIssue(f"config.h missing required macro: {macro}"))
    return issues


def find_issues_for_uoff_t_selector(config_h: str) -> list[ContractIssue]:
    for macro in UOFF_T_SELECTOR_MACROS:
        if re.search(rf"^\s*#\s*define\s+{re.escape(macro)}\b", config_h, re.MULTILINE):
            return []

    return [
        ContractIssue(
            "config.h must define one uoff_t selector macro: " +
            ", ".join(UOFF_T_SELECTOR_MACROS),
        ),
    ]


def find_issues_for_abi_version(config_h: str) -> list[ContractIssue]:
    match = re.search(
        r'^\s*#\s*define\s+DOVECOT_ABI_VERSION\s+"([^"]+)"',
        config_h,
        re.MULTILINE,
    )
    if match is not None and match.group(1) == PINNED_DOVECOT_ABI_VERSION:
        return []

    return [
        ContractIssue(
            "config.h DOVECOT_ABI_VERSION must equal "
            f'"{PINNED_DOVECOT_ABI_VERSION}"',
        ),
    ]


def validate_mailbox_vfunc_probe(
    source_dir: Path,
    build_dir: Path,
) -> list[ContractIssue]:
    cc_spec = os.environ.get("CC", "cc")
    try:
        cc_argv = shlex.split(cc_spec)
    except ValueError as error:
        return [ContractIssue(
            f"failed to parse CC value for compiler probe: {error}",
        )]
    if not cc_argv:
        return [ContractIssue("no C compiler available for configured-header probe")]

    compiler = shutil.which(cc_argv[0])
    if compiler is None:
        return [ContractIssue(
            f"no C compiler available for configured-header probe: {cc_argv[0]}",
        )]

    with tempfile.TemporaryDirectory() as build_tmp_dir:
        probe_path = Path(build_tmp_dir) / "mailbox-vfunc-probe.c"
        probe_path.write_text(MAILBOX_VFUNC_PROBE_SOURCE, encoding="utf-8")

        command = [
            compiler,
            *cc_argv[1:],
            "-std=gnu11",
            "-fsyntax-only",
            "-Werror=incompatible-pointer-types",
            f"-I{build_dir}",
            f"-I{source_dir / 'src' / 'lib-index'}",
            f"-I{source_dir / 'src' / 'lib'}",
            f"-I{source_dir / 'src' / 'lib-mail'}",
            f"-I{source_dir / 'src' / 'lib-storage'}",
        ]
        source = str(probe_path)
        process = subprocess.run(
            command + [source],
            text=True,
            capture_output=True,
            check=False,
        )

    if process.returncode == 0:
        return []

    return [
        ContractIssue(
            "configured-header mailbox vfunc probe failed:\n"
            f"{process.stderr or process.stdout}".strip() or "unknown failure",
        ),
    ]


def validate_build_config(source_dir: Path, build_dir: Path) -> list[ContractIssue]:
    issues: list[ContractIssue] = []
    if (source_dir / "src" / "lib" / "module-dir.h").is_file() is False:
        issues.append(ContractIssue("source directory is missing src/lib/module-dir.h"))
    if (source_dir / "src" / "lib" / "lib.h").is_file() is False:
        issues.append(ContractIssue("source directory is missing src/lib/lib.h"))

    config_h_path = build_dir / "config.h"
    if not config_h_path.is_file():
        issues.append(ContractIssue(
            f"build directory is missing required config.h: {config_h_path}",
        ))
        return issues

    config_h_text = read_text(config_h_path)
    issues.extend(find_issues_for_required_macros(config_h_text, REQUIRED_CONFIG_MACROS))
    issues.extend(find_issues_for_abi_version(config_h_text))
    issues.extend(find_issues_for_uoff_t_selector(config_h_text))
    issues.extend(validate_mailbox_vfunc_probe(source_dir, build_dir))
    return issues


def main() -> int:
    args = parse_args()
    try:
        source_dir = resolve_dir(args.source_dir, "source")
        build_dir = resolve_dir(args.build_dir, "build")
        issues = validate_build_config(source_dir, build_dir)
    except (OSError, ValueError) as error:
        print(f"dovecot build contract check failed: {error}", file=sys.stderr)
        return 1

    if issues:
        print("dovecot build contract check failed", file=sys.stderr)
        for issue in issues:
            print(f" - {issue.message}", file=sys.stderr)
        return 1

    print(
        f"dovecot build contract check passed: source={source_dir}, build={build_dir}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
