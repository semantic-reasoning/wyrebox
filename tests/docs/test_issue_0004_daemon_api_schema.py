#!/usr/bin/env python3

import argparse
from pathlib import Path
import shutil
import subprocess


REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "wyrebox" / "wyrebox-daemon-api.capnp"
CAPNP_OUTPUT_FLAG = "-ocapnp"

REQUIRED_TOP_LEVEL = [
    "struct RequestFrame",
    "struct ResponseFrame",
    "struct RequestIdentity",
    "struct SuccessFrame",
    "struct ErrorFrame",
    "struct StreamChunkFrame",
    "enum ErrorClass",
]

REQUIRED_IDENTITY_FIELDS = [
    "requestId",
    "deliveryId",
    "callerIdentity",
    "toolIdentity",
    "accountIdentity",
    "correlationId",
    "durableMarker",
    "journalOffset",
    "journalSequence",
]

REQUIRED_ERROR_CLASSES = [
    "temporaryFailure",
    "permanentFailure",
    "permissionDenied",
    "notFound",
    "conflict",
    "internalError",
]

REQUIRED_OPERATION_GROUPS = [
    "deliveryIngestion",
    "struct DeliveryIngestionRequest",
    "mailboxList",
    "struct MailboxListRequest",
    "mailboxSelect",
    "struct MailboxSelectRequest",
    "messageFetch",
    "struct MessageFetchRequest",
    "messageSearch",
    "struct MessageSearchRequest",
    "flagKeywordUpdate",
    "struct FlagKeywordUpdateRequest",
    "factMutation",
    "struct FactMutationRequest",
    "wirelogPredicateQuery",
    "struct WirelogPredicateQueryRequest",
    "duckDBQueryTemplate",
    "struct DuckDBQueryTemplateRequest",
]

REQUIRED_STREAM_FIELDS = [
    "messageId",
    "queryId",
    "chunkIndex",
    "bytes",
    "endOfStream",
]

FORBIDDEN_IMPLEMENTATION_TERMS = [
    "daemon implementation",
    "generated C integration",
    "DuckDB execution code",
    "Wirelog integration code",
    "storage implementation",
]


def assert_contains_all(text: str, needles: list[str], label: str) -> None:
    missing = [needle for needle in needles if needle not in text]
    assert not missing, f"missing {label}: " + ", ".join(missing)


def assert_success_frame_fields(text: str) -> None:
    start = text.index("struct SuccessFrame {")
    end = text.index("\n}", start)
    success_frame = text[start:end]

    expected_fields = [
        "requestId @0 :Text;",
        "durableMarker @1 :Text;",
        "journalOffset @2 :UInt64;",
        "summary @3 :Text;",
        "journalSequence @4 :UInt64;",
    ]
    assert_contains_all(success_frame, expected_fields, "success frame fields")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--capnp-mode",
        choices=("auto", "validate", "skip"),
        default="auto",
        help=(
            "schema compiler validation policy; Meson passes validate or skip, "
            "auto is for direct script execution"
        ),
    )
    parser.add_argument(
        "--capnp-path",
        help="path to capnp when --capnp-mode=validate",
    )
    return parser.parse_args()


def capnp_compile_command(capnp: str) -> list[str]:
    command = [
        capnp,
        "compile",
        CAPNP_OUTPUT_FLAG,
        str(SCHEMA_PATH.relative_to(REPO_ROOT)),
    ]
    assert "-o-" not in command, "capnp validation must not use the null output flag"
    return command


def resolve_capnp_path(args: argparse.Namespace) -> str | None:
    if args.capnp_mode == "skip":
        print("capnp mode skip; skipping schema compiler syntax validation")
        return None

    if args.capnp_mode == "validate":
        assert args.capnp_path, "--capnp-mode=validate requires --capnp-path"
        return args.capnp_path

    capnp = shutil.which("capnp")
    if capnp is None:
        print(
            "capnp mode auto; capnp not found; "
            "skipping schema compiler syntax validation"
        )
        return None

    print(f"capnp mode auto; validating schema with {capnp}")
    return capnp


def run_capnp_syntax_check(capnp: str) -> None:
    command = capnp_compile_command(capnp)

    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, (
        "capnp schema syntax validation failed\n"
        f"stdout:\n{result.stdout}\n"
        f"stderr:\n{result.stderr}"
    )


def main() -> None:
    args = parse_args()

    assert SCHEMA_PATH.is_file(), f"missing schema: {SCHEMA_PATH}"

    text = SCHEMA_PATH.read_text(encoding="utf-8")

    assert_contains_all(text, REQUIRED_TOP_LEVEL, "top-level schema types")
    assert_contains_all(text, REQUIRED_IDENTITY_FIELDS, "identity and durability fields")
    assert_success_frame_fields(text)
    assert_contains_all(text, REQUIRED_ERROR_CLASSES, "error classes")
    assert_contains_all(text, REQUIRED_OPERATION_GROUPS, "operation groups")
    assert_contains_all(text, REQUIRED_STREAM_FIELDS, "stream/chunk fields")

    forbidden = [term for term in FORBIDDEN_IMPLEMENTATION_TERMS if term in text]
    assert not forbidden, "schema contains forbidden terms: " + ", ".join(forbidden)

    assert "-o-" not in capnp_compile_command("capnp")

    capnp = resolve_capnp_path(args)
    if capnp is not None:
        run_capnp_syntax_check(capnp)


if __name__ == "__main__":
    main()
