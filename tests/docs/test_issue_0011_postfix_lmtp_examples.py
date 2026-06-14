#!/usr/bin/env python3

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_DIR = REPO_ROOT / "examples" / "postfix"
README_PATH = EXAMPLE_DIR / "README.wyrebox-lmtp.md"
MASTER_CF_PATH = EXAMPLE_DIR / "master.cf.wyrebox-lmtp"
TRANSPORT_PATH = EXAMPLE_DIR / "transport.wyrebox-lmtp"


def read_examples() -> dict[Path, str]:
    paths = [README_PATH, MASTER_CF_PATH, TRANSPORT_PATH]
    for path in paths:
        assert path.is_file(), f"missing example file: {path}"
    return {path: path.read_text(encoding="utf-8") for path in paths}


def squashed(text: str) -> str:
    return " ".join(text.split())


def assert_current_single_recipient_contract(examples: dict[Path, str]) -> None:
    combined = "\n".join(examples.values())

    assert "wyrebox-lmtp_destination_recipient_limit = 1" in combined
    assert "one accepted recipient per LMTP transaction" in examples[README_PATH]
    assert "Postfix must split recipients before delivery" in examples[README_PATH]
    assert "multiple `RCPT TO` commands in one transaction are rejected" in (
        examples[README_PATH]
    )
    assert "recipient-level mixed outcomes are future work" in examples[README_PATH]


def assert_lmtp_transport_shape(master_cf: str) -> None:
    required_fragments = [
        "wyrebox-lmtp unix  -       -       n       -       -       lmtp",
        "wyrebox-lmtp_destination_recipient_limit = 1",
    ]

    for fragment in required_fragments:
        assert fragment in master_cf, f"missing master.cf fragment: {fragment}"

    assert "argv=/usr/local/bin/wyrebox-postfix-lmtp" not in master_cf
    assert "argv=" not in master_cf


def assert_no_full_multi_recipient_claims(examples: dict[Path, str]) -> None:
    forbidden = [
        "full multi-recipient LMTP",
        "partial recipient status is supported",
        "recipient-level mixed outcomes are supported",
        "multiple recipients per transaction are supported",
    ]
    combined = "\n".join(examples.values()).lower()

    for phrase in forbidden:
        assert phrase.lower() not in combined, f"forbidden claim: {phrase}"


def main() -> None:
    examples = read_examples()
    combined = "\n".join(examples.values())

    assert "`wyrebox-postfix-lmtp`" in examples[README_PATH]
    assert "Postfix LMTP transport requires a WyreBox LMTP listener endpoint" in (
        examples[README_PATH]
    )
    assert "wyrebox-lmtp:unix:/run/wyrebox/wyrebox-lmtp.sock" in (
        examples[TRANSPORT_PATH]
    )
    assert "is not a Postfix `lmtp(8)` transport command" in squashed(
        examples[README_PATH]
    )
    assert_current_single_recipient_contract(examples)
    assert_lmtp_transport_shape(examples[MASTER_CF_PATH])
    assert_no_full_multi_recipient_claims(examples)
    assert "/run/wyrebox/wyrebox.sock" in combined
    assert "/run/wyrebox/wyrebox-lmtp.sock" in combined


if __name__ == "__main__":
    main()
