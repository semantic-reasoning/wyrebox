#!/usr/bin/env python3

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = REPO_ROOT / "docs" / "adr" / "0001-postfix-ingress-boundary.md"

REQUIRED_SECTIONS = [
    "# ADR 0001: Postfix Ingress Boundary",
    "## Status",
    "## Context",
    "## Decision",
    "## Helper Input Expectations",
    "## Envelope Metadata Assumptions",
    "## Durable Ingestion Success Boundary",
    "## Failure Mapping",
    "## Duplicate Delivery Risk",
    "## Chroot And Privilege-Drop Implications",
    "## Daemon Socket Dependency",
    "## State Mutation Boundary",
    "## Deferred Decisions",
    "## Consequences",
]

REQUIRED_TERMS = [
    "pipe(8)",
    "LMTP",
    "additive",
    "Dovecot LMTP",
    "WyreBox Dovecot storage backend",
    "RFC 5322",
    "queue ID",
    "sender",
    "recipient",
    "raw object storage",
    "journal append",
    "temporary failure",
    "permanent failure",
    "duplicate delivery",
    "chroot",
    "privilege drop",
    "/run/wyrebox/wyrebox.sock",
    "DuckDB",
    "Wirelog",
    "object-store metadata",
    "canonical mutation journal",
    "wyreboxd",
]


def main() -> None:
    assert ADR_PATH.is_file(), f"missing ADR: {ADR_PATH}"

    text = ADR_PATH.read_text(encoding="utf-8")

    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    missing_terms = [term for term in REQUIRED_TERMS if term not in text]
    assert not missing_terms, "missing required terms: " + ", ".join(missing_terms)

    assert "LMTP ingress is later additive work, not a replacement decision now." in text
    assert "Dovecot storage-backend decisions are deferred to ADR 0002" in text


if __name__ == "__main__":
    main()
