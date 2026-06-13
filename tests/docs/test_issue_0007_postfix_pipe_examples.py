#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_DIR = REPO_ROOT / "examples" / "postfix"
README_PATH = EXAMPLE_DIR / "README.md"
MASTER_CF_PATH = EXAMPLE_DIR / "master.cf.wyrebox-pipe"
TRANSPORT_PATH = EXAMPLE_DIR / "transport.wyrebox-pipe"

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
    paths = [README_PATH, MASTER_CF_PATH, TRANSPORT_PATH]
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


def main() -> None:
    examples = read_examples()
    combined = "\n".join(examples.values())
    lower_combined = combined.lower()

    assert "`wyrebox-postfix-pipe`" in examples[README_PATH]
    assert "argv=/usr/local/bin/wyrebox-postfix-pipe" in examples[MASTER_CF_PATH]

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

    for claim in FORBIDDEN_CLAIMS:
        assert claim not in lower_combined, f"forbidden claim present: {claim}"


if __name__ == "__main__":
    main()
