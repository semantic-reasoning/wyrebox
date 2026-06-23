#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_DIR = REPO_ROOT / "examples" / "postfix"
README_PATH = EXAMPLE_DIR / "README.md"
MASTER_CF_PATH = EXAMPLE_DIR / "master.cf.wyrebox-pipe"
TRANSPORT_PATH = EXAMPLE_DIR / "transport.wyrebox-pipe"
REGEXP_TRANSPORT_PATH = EXAMPLE_DIR / "transport.wyrebox-pipe-regexp"

SUPPORTED_OPTIONS = {
    "--account-id",
    "--delivery-id",
    "--queue-id",
    "--sender",
    "--recipient",
    "--socket",
}

FORBIDDEN_CLAIMS = [
    "duckdb",
    "object-store",
    "object store",
    "tcp",
    "lmtp",
    "duplicate suppression",
    "deduplicate",
    "deduplication",
]


def read_examples() -> dict[Path, str]:
    paths = [
        README_PATH,
        MASTER_CF_PATH,
        TRANSPORT_PATH,
        REGEXP_TRANSPORT_PATH,
    ]
    for path in paths:
        assert path.is_file(), f"missing example file: {path}"
    return {path: path.read_text(encoding="utf-8") for path in paths}


def assert_supported_options_only(text: str) -> None:
    options = set(re.findall(r"--[a-z0-9-]+", text))
    unsupported = options - SUPPORTED_OPTIONS
    missing = SUPPORTED_OPTIONS - options

    assert not unsupported, "unsupported helper options: " + ", ".join(
        sorted(unsupported)
    )
    assert not missing, "missing documented helper options: " + ", ".join(
        sorted(missing)
    )


def assert_contains_words(text: str, phrase: str) -> None:
    normalized_text = " ".join(text.split())
    normalized_phrase = " ".join(phrase.split())

    assert normalized_phrase in normalized_text, f"missing phrase: {phrase}"


def assert_pipe_flags_preserve_stdin(master_cf: str) -> None:
    match = re.search(r"^\s*flags=([^\s]+)", master_cf, flags=re.MULTILINE)

    assert match is not None, "master.cf example must set explicit pipe flags"

    flags = set(match.group(1))
    assert "R" not in flags, "pipe R flag prepends Return-Path and mutates stdin"
    assert flags == {"q"}, "pipe example must use only non-mutating q flag"


def assert_single_recipient_pipe_delivery(master_cf: str) -> None:
    assert "wyrebox-pipe_destination_recipient_limit = 1" in master_cf
    assert "--recipient ${recipient}" in master_cf


def assert_required_master_cf_metadata(master_cf: str) -> None:
    required_fragments = [
        "--account-id example-account",
        "--delivery-id ${queue_id}:${recipient}",
        "--queue-id ${queue_id}",
        "--sender ${sender}",
        "--recipient ${recipient}",
    ]

    for fragment in required_fragments:
        assert fragment in master_cf, f"missing master.cf metadata: {fragment}"


def main() -> None:
    examples = read_examples()
    combined = "\n".join(examples.values())
    lower_combined = combined.lower()

    assert "`wyrebox-postfix-pipe`" in examples[README_PATH]
    assert "argv=/usr/local/bin/wyrebox-postfix-pipe" in examples[MASTER_CF_PATH]
    assert_pipe_flags_preserve_stdin(examples[MASTER_CF_PATH])
    assert_single_recipient_pipe_delivery(examples[MASTER_CF_PATH])
    assert_required_master_cf_metadata(examples[MASTER_CF_PATH])

    assert_supported_options_only(combined)

    assert re.search(
        r"--recipient\s+\S+\s+\\\n\s+--recipient\s+\S+",
        examples[README_PATH],
    ), "README must demonstrate repeated --recipient arguments"

    assert "/run/wyrebox/wyrebox.sock" in combined
    assert "--socket /path/to/wyrebox.sock" in examples[README_PATH]

    assert_contains_words(
        examples[README_PATH],
        "raw RFC 5322 message bytes from standard input",
    )
    assert_contains_words(
        examples[MASTER_CF_PATH],
        "stdin carries the raw RFC 5322 message bytes",
    )
    assert_contains_words(
        examples[README_PATH],
        "preserves the bytes it receives on standard input",
    )
    assert_contains_words(
        examples[README_PATH],
        "must not use pipe flags that alter message content",
    )
    assert_contains_words(
        examples[README_PATH],
        "q flag affects command-line address macro expansion only",
    )

    assert "returns success only after `wyreboxd` reports durable ingestion" in (
        examples[README_PATH]
    )
    assert "return temporary failure so Postfix may retry" in examples[README_PATH]
    assert "return permanent failure" in examples[README_PATH]

    assert "does not\nlog raw message bytes" in examples[README_PATH]
    assert "socket path is visible inside the chroot" in examples[README_PATH]

    assert "Postfix may retry the delivery" in examples[README_PATH]
    assert "Duplicate and idempotency policy is owned by daemon ingestion" in (
        examples[README_PATH]
    )

    assert "distro-compatible transport map" in examples[README_PATH]
    assert "transport.wyrebox-pipe-regexp" in examples[README_PATH]
    assert "hash:/etc/postfix/transport" not in examples[README_PATH]
    assert "regexp:/etc/postfix/transport" in examples[REGEXP_TRANSPORT_PATH]
    assert "wyrebox-pipe:" in examples[REGEXP_TRANSPORT_PATH]

    for claim in FORBIDDEN_CLAIMS:
        assert claim not in lower_combined, f"forbidden claim present: {claim}"


if __name__ == "__main__":
    main()
