#!/usr/bin/env python3

from pathlib import Path
import shutil
import subprocess


REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "wyrebox" / "wyrebox-daemon-api.capnp"

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


def run_capnp_syntax_check() -> None:
    capnp = shutil.which("capnp")
    if capnp is None:
        print("capnp not found; skipping schema syntax validation")
        return

    result = subprocess.run(
        [capnp, "compile", "-ocapnp", str(SCHEMA_PATH.relative_to(REPO_ROOT))],
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
    assert SCHEMA_PATH.is_file(), f"missing schema: {SCHEMA_PATH}"

    text = SCHEMA_PATH.read_text(encoding="utf-8")

    assert_contains_all(text, REQUIRED_TOP_LEVEL, "top-level schema types")
    assert_contains_all(text, REQUIRED_IDENTITY_FIELDS, "identity and durability fields")
    assert_contains_all(text, REQUIRED_ERROR_CLASSES, "error classes")
    assert_contains_all(text, REQUIRED_OPERATION_GROUPS, "operation groups")
    assert_contains_all(text, REQUIRED_STREAM_FIELDS, "stream/chunk fields")

    forbidden = [term for term in FORBIDDEN_IMPLEMENTATION_TERMS if term in text]
    assert not forbidden, "schema contains forbidden terms: " + ", ".join(forbidden)

    run_capnp_syntax_check()


if __name__ == "__main__":
    main()
