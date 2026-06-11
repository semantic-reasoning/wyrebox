#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "core-daemon-api.md"

REQUIRED_SECTIONS = [
    "# Core Daemon API Contract",
    "## Status",
    "## Scope",
    "## Unix Domain Socket Transport",
    "## Local Access And Peer Identity",
    "## Cap'n Proto Framing Boundary",
    "## Request Identity And Correlation",
    "## Caller-Observed Success Semantics",
    "## Delivery Ingestion Operation Contract",
    "## Mailbox List Select Operation Contract",
    "## Message Fetch Operation Contract",
    "## State Authority Boundary",
    "## Deferred Operation Payloads",
    "## Out Of Scope",
]

DELIVERY_FORBIDDEN_SURFACES = [
    r"arbitrary SQL",
    r"write SQL",
    r"DuckDB mutation",
    r"Wirelog fact mutation",
    r"object-store metadata mutation",
    r"direct journal append",
]

DELIVERY_FORBIDDEN_SURFACE_VERBS = (
    r"exposes?|allows?|permits?|provides?|accepts?|supports?|executes?|enables?|grants?"
)

DELIVERY_FORBIDDEN_SURFACE_PATTERNS = [
    rf"(?<!not )\b(?:{DELIVERY_FORBIDDEN_SURFACE_VERBS})\b(?:\s+\S+){{0,8}}\s+{surface}"
    for surface in DELIVERY_FORBIDDEN_SURFACES
]

DELIVERY_FORBIDDEN_MODAL_PATTERNS = [
    rf"\b(may|can|must|should)\b\s+\b(expose|allow|permit|provide|accept|support|execute|enable|grant)\b.*{surface}"
    for surface in DELIVERY_FORBIDDEN_SURFACES
]

DELIVERY_FORBIDDEN_EXAMPLES = [
    "Delivery ingestion exposes arbitrary SQL.",
    "Delivery ingestion accepts arbitrary SQL from helpers.",
    "Delivery ingestion supports write SQL for helpers.",
    "Delivery ingestion executes arbitrary SQL supplied by helpers.",
    "Delivery ingestion enables DuckDB mutation by helpers.",
    "Delivery ingestion exposes Wirelog fact mutation surfaces.",
    "Delivery ingestion provides object-store metadata mutation access.",
    "Delivery ingestion grants helpers direct journal append access.",
]

MAILBOX_FORBIDDEN_SURFACES = [
    r"arbitrary SQL",
    r"write SQL",
    r"DuckDB mutation",
    r"Wirelog mutation",
    r"object-store metadata mutation",
    r"direct journal append",
    r"direct journal write",
]

MAILBOX_FORBIDDEN_SURFACE_PATTERNS = [
    rf"(?<!not )\b(?:{DELIVERY_FORBIDDEN_SURFACE_VERBS})\b(?:\s+\S+){{0,8}}\s+{surface}"
    for surface in MAILBOX_FORBIDDEN_SURFACES
]

MAILBOX_FORBIDDEN_MODAL_PATTERNS = [
    rf"\b(may|can|must|should)\b\s+\b(expose|allow|permit|provide|accept|support|execute|enable|grant)\b.*{surface}"
    for surface in MAILBOX_FORBIDDEN_SURFACES
]

MAILBOX_FORBIDDEN_EXAMPLES = [
    "Mailbox LIST exposes arbitrary SQL.",
    "Mailbox SELECT accepts write SQL from Dovecot.",
    "Mailbox LIST enables DuckDB mutation by plugins.",
    "Mailbox SELECT supports Wirelog mutation by plugins.",
    "Mailbox LIST provides object-store metadata mutation access.",
    "Mailbox SELECT grants direct journal append access.",
    "Mailbox SELECT allows direct journal write access.",
    "Mailbox SELECT may mutate DuckDB.",
    "Mailbox SELECT may mutate Wirelog.",
    "Mailbox SELECT may mutate object-store metadata.",
    "Mailbox SELECT may mutate journal state.",
    "Mailbox SELECT may append mutation journal records.",
    "Mailbox SELECT may write directly to the journal.",
]

MAILBOX_FORBIDDEN_DIRECT_ACTION_PATTERNS = [
    r"\b(may|can|must|should|will)\b\s+mutate\s+DuckDB",
    r"\b(may|can|must|should|will)\b\s+mutate\s+Wirelog",
    r"\b(may|can|must|should|will)\b\s+mutate\s+object-store metadata",
    r"\b(may|can|must|should|will)\b\s+mutate\s+journal state",
    r"\b(may|can|must|should|will)\b\s+append\s+mutation journal records",
    r"\b(may|can|must|should|will)\b\s+write\s+directly\s+to\s+the\s+journal",
]

FETCH_FORBIDDEN_SURFACES = [
    r"arbitrary SQL",
    r"write SQL",
    r"DuckDB mutation",
    r"Wirelog mutation",
    r"object-store metadata mutation",
    r"raw object rewrite",
    r"raw message byte rewrite",
    r"direct journal append",
    r"direct journal write",
    r"mutation journal append surface",
]

FETCH_FORBIDDEN_SURFACE_PATTERNS = [
    rf"(?<!not )\b(?:{DELIVERY_FORBIDDEN_SURFACE_VERBS})\b(?:\s+\S+){{0,8}}\s+{surface}"
    for surface in FETCH_FORBIDDEN_SURFACES
]

FETCH_FORBIDDEN_MODAL_PATTERNS = [
    rf"\b(may|can|must|should)\b\s+\b(expose|allow|permit|provide|accept|support|execute|enable|grant)\b.*{surface}"
    for surface in FETCH_FORBIDDEN_SURFACES
]

FETCH_FORBIDDEN_DIRECT_ACTION_PATTERNS = [
    r"\b(may|can|must|should|will)\b\s+mutate\s+DuckDB",
    r"\b(may|can|must|should|will)\b\s+mutate\s+Wirelog",
    r"\b(may|can|must|should|will)\b\s+mutate\s+object-store metadata",
    r"\b(may|can|must|should|will)\b\s+mutate\s+raw objects",
    r"\b(may|can|must|should|will)\b\s+mutate\s+journal state",
    r"\b(may|can|must|should|will)\b\s+rewrite\s+raw message bytes",
    r"\b(may|can|must|should|will)\b\s+append\s+mutation journal records",
    r"\b(may|can|must|should|will)\b\s+append\s+to\s+the\s+mutation journal",
    r"\b(may|can|must|should|will)\b\s+write\s+directly\s+to\s+the\s+journal",
]

FETCH_FORBIDDEN_EXAMPLES = [
    "FETCH exposes arbitrary SQL.",
    "FETCH accepts write SQL from Dovecot.",
    "FETCH enables DuckDB mutation by plugins.",
    "FETCH supports Wirelog mutation by plugins.",
    "FETCH provides object-store metadata mutation access.",
    "FETCH grants direct journal append access.",
    "FETCH allows direct journal write access.",
    "FETCH exposes a mutation journal append surface.",
    "FETCH may mutate DuckDB.",
    "FETCH may mutate Wirelog.",
    "FETCH may mutate object-store metadata.",
    "FETCH may mutate raw objects.",
    "FETCH may mutate journal state.",
    "FETCH may rewrite raw message bytes.",
    "FETCH may append mutation journal records.",
    "FETCH may append to the mutation journal.",
    "FETCH may write directly to the journal.",
]


def section_map(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"^## .+$", text, flags=re.MULTILINE))
    sections: dict[str, str] = {}

    for index, match in enumerate(matches):
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections[match.group(0)] = text[start:end].strip()

    return sections


def assert_in_section(sections: dict[str, str], section: str, needle: str) -> None:
    assert needle in sections[section], f"{section} missing text: {needle}"


def assert_section_matches(sections: dict[str, str], section: str, pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(
        flexible_pattern, sections[section], re.IGNORECASE | re.DOTALL
    ), f"{section} missing pattern: {pattern}"


def assert_forbidden(text: str, pattern: str) -> None:
    assert not re.search(pattern, text, re.IGNORECASE | re.DOTALL), (
        f"contract contains forbidden language: {pattern}"
    )


def assert_section_forbidden(
    sections: dict[str, str], section: str, pattern: str
) -> None:
    assert not re.search(pattern, sections[section], re.IGNORECASE | re.DOTALL), (
        f"{section} contains forbidden language: {pattern}"
    )


def assert_any_pattern_matches(text: str, patterns: list[str]) -> None:
    assert any(
        re.search(pattern, text, re.IGNORECASE | re.DOTALL) for pattern in patterns
    ), f"forbidden example was not rejected by guard patterns: {text}"


def main() -> None:
    assert CONTRACT_PATH.is_file(), f"missing contract: {CONTRACT_PATH}"

    text = CONTRACT_PATH.read_text(encoding="utf-8")
    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    sections = section_map(text)

    assert_section_matches(sections, "## Status", r"Accepted for issue 0004")

    assert_section_matches(
        sections,
        "## Scope",
        r"command payload schemas",
    )
    assert_section_matches(
        sections,
        "## Scope",
        r"Delivery ingestion operation contract",
    )
    assert_section_matches(
        sections,
        "## Scope",
        r"Mailbox LIST/SELECT operation contract",
    )
    assert_section_matches(
        sections,
        "## Scope",
        r"Message FETCH operation contract",
    )
    assert_section_matches(
        sections,
        "## Scope",
        r"does not define concrete command payload schemas, daemon runtime internals, or command query implementation",
    )
    assert_section_matches(
        sections,
        "## Scope",
        r"Error-class semantics are defined by `docs/contracts/error-model.md`",
    )

    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"/run/wyrebox/wyrebox.sock",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"Unix domain socket only",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"No TCP listener",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"No HTTP endpoint",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"No remote authentication model",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"No LMTP transport",
    )

    assert_section_matches(
        sections,
        "## Local Access And Peer Identity",
        r"Postfix helpers, Dovecot plugins, and local tools/skills",
    )
    assert_section_matches(
        sections,
        "## Local Access And Peer Identity",
        r"peer credential",
    )
    assert_section_matches(
        sections,
        "## Local Access And Peer Identity",
        r"group-based authorization",
    )

    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"request frame",
    )
    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"response frame",
    )
    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"error frame",
    )
    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"stream/chunk frame",
    )
    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"categories.*not concrete field layouts",
    )

    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"request_id",
    )
    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"delivery_id",
    )
    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"queue identifiers",
    )
    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"IMAP operation correlation",
    )
    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"journal_offset",
    )

    assert_section_matches(
        sections,
        "## Caller-Observed Success Semantics",
        r"definitive daemon success response",
    )
    assert_section_matches(
        sections,
        "## Caller-Observed Success Semantics",
        r"Ambiguous transport outcomes are not caller success",
    )
    assert_section_matches(
        sections,
        "## Caller-Observed Success Semantics",
        r"not.*silently promoted",
    )

    for required_field in [
        "request_id",
        "delivery_id",
        "queue ID",
        "envelope sender",
        "recipients",
    ]:
        assert_section_matches(
            sections,
            "## Delivery Ingestion Operation Contract",
            required_field,
        )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"Cap'n Proto-over-UDS",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"does not define concrete `.capnp` field layouts or generated code",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"raw RFC 5322 message payload has an explicit transfer boundary",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"exact payload bytes.*API framing.*canonical original message object",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"must not rewrite raw message bytes",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"success response.*only after.*durable",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"durable raw object-store commit.*durable append.*mutation journal",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"journal_offset.*equivalent durable marker",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"docs/contracts/error-model.md",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"temporary failures are retryable",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"permanent failure.*explicit non-retryable validation, configuration, or policy errors",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"ambiguous communication is not delivery success",
    )
    assert_section_matches(
        sections,
        "## Delivery Ingestion Operation Contract",
        r"does not expose arbitrary SQL, write SQL, DuckDB mutation, Wirelog fact mutation, object-store metadata mutation, or direct journal append",
    )

    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"Dovecot-facing LIST and SELECT calls",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"Cap'n Proto-over-UDS",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"does not define concrete `.capnp` schemas, field layouts, generated code, or Dovecot backend implementation",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"caller/account identity.*scope mailbox visibility",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"must not return mailbox records outside the authorized account scope",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"ordinary mailboxes.*stable mailbox_id",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"virtual mailboxes.*stable view_id",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"Virtual mailbox identity is first-class.*not a client-side fiction",
    )
    for list_field in [
        "stable identifier",
        "IMAP-visible name",
        "hierarchy delimiter",
        "children",
        "selectable",
        "ordinary",
        "virtual",
    ]:
        assert_section_matches(
            sections,
            "## Mailbox List Select Operation Contract",
            list_field,
        )
    for select_field in [
        "UIDVALIDITY",
        "UIDNEXT",
        "selected-mailbox state",
        "ordinary mailbox or virtual mailbox",
    ]:
        assert_section_matches(
            sections,
            "## Mailbox List Select Operation Contract",
            select_field,
        )
    for error_case in [
        "not found",
        "authorization",
        "temporary backend failure",
        "docs/contracts/error-model.md",
    ]:
        assert_section_matches(
            sections,
            "## Mailbox List Select Operation Contract",
            error_case,
        )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"non-selectable.*Conflict.*selection-state conflict.*not success",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"read-only",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"do not append mutation journal records",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"do not mutate DuckDB, Wirelog, object-store metadata, or journal state",
    )
    assert_section_matches(
        sections,
        "## Mailbox List Select Operation Contract",
        r"does not expose arbitrary SQL, write SQL, DuckDB mutation, Wirelog mutation, object-store metadata mutation, direct journal append, or direct journal write",
    )

    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"Dovecot-facing FETCH",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"Cap'n Proto-over-UDS",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"does not define concrete `.capnp` schemas, field layouts, generated code, or Dovecot backend implementation",
    )
    for identity_field in [
        "request_id",
        "IMAP operation correlation",
        "account identity",
        "selected mailbox",
        "UID",
        "message reference",
    ]:
        assert_section_matches(
            sections,
            "## Message Fetch Operation Contract",
            identity_field,
        )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"caller/account identity.*scope",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"must not return messages outside the authorized selected mailbox or virtual mailbox view",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"ordinary mailbox",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"virtual mailbox",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"stable selected-mailbox identity",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"byte-for-byte original RFC 5322 message",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"canonical object",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"must not rewrite raw bytes.*flags, facts, search, or virtual views",
    )
    for stream_field in [
        "stream/chunk",
        "chunk",
        "request_id",
        "message",
        "end",
        "error",
    ]:
        assert_section_matches(
            sections,
            "## Message Fetch Operation Contract",
            stream_field,
        )
    for error_case in [
        "not found",
        "permission denied",
        "conflict",
        "temporary backend failure",
        "docs/contracts/error-model.md",
        "ambiguous transport outcomes are not success",
    ]:
        assert_section_matches(
            sections,
            "## Message Fetch Operation Contract",
            error_case,
        )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"read-only",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"does not append mutation journal records",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"does not mutate DuckDB, Wirelog, object-store metadata, raw objects, or journal state",
    )
    assert_section_matches(
        sections,
        "## Message Fetch Operation Contract",
        r"does not expose arbitrary SQL, write SQL, DuckDB mutation, Wirelog mutation, object-store metadata mutation, raw object rewrite, direct journal append, direct journal write, or mutation journal append surfaces",
    )

    assert_section_matches(
        sections,
        "## State Authority Boundary",
        r"wyreboxd.*only mutable owner",
    )
    assert_section_matches(
        sections,
        "## State Authority Boundary",
        r"Postfix helpers.*Dovecot plugins.*local tools and skills",
    )
    assert_section_matches(
        sections,
        "## State Authority Boundary",
        r"must not open or mutate DuckDB materialized state, Wirelog state, object-store metadata, or canonical journal state directly",
    )
    assert_section_matches(
        sections,
        "## State Authority Boundary",
        r"GLib/GObject implementation style",
    )

    for operation in [
        "flag/keyword update",
        "search",
        "fact insert/retract",
        "Wirelog predicate query",
        "safe DuckDB query templates",
    ]:
        assert_in_section(sections, "## Deferred Operation Payloads", operation)
    assert_section_matches(
        sections,
        "## Deferred Operation Payloads",
        r"no arbitrary write SQL",
    )

    for excluded in [
        "Dovecot search",
        "Dovecot implementation",
        "fact/query APIs",
        "concrete daemon implementation",
        "full .capnp generation",
    ]:
        assert_in_section(sections, "## Out Of Scope", excluded)

    for excluded in [
        "concrete Cap'n Proto message layouts",
        "daemon internals",
        "public remote API",
        "TLS",
        "HTTP",
        "TCP",
        "remote authentication",
        "LMTP",
    ]:
        assert_in_section(sections, "## Out Of Scope", excluded)

    for forbidden in [
        r"command-specific schema",
        r"full SQL support",
    ]:
        assert_forbidden(text, forbidden)

    for pattern in DELIVERY_FORBIDDEN_SURFACE_PATTERNS:
        assert_section_forbidden(
            sections,
            "## Delivery Ingestion Operation Contract",
            pattern,
        )
    for pattern in DELIVERY_FORBIDDEN_MODAL_PATTERNS:
        assert_section_forbidden(
            sections,
            "## Delivery Ingestion Operation Contract",
            pattern,
        )
    for example in DELIVERY_FORBIDDEN_EXAMPLES:
        assert_any_pattern_matches(
            sections["## Delivery Ingestion Operation Contract"] + "\n" + example,
            DELIVERY_FORBIDDEN_SURFACE_PATTERNS + DELIVERY_FORBIDDEN_MODAL_PATTERNS,
        )
    for pattern in MAILBOX_FORBIDDEN_SURFACE_PATTERNS:
        assert_section_forbidden(
            sections,
            "## Mailbox List Select Operation Contract",
            pattern,
        )
    for pattern in MAILBOX_FORBIDDEN_MODAL_PATTERNS:
        assert_section_forbidden(
            sections,
            "## Mailbox List Select Operation Contract",
            pattern,
        )
    for pattern in MAILBOX_FORBIDDEN_DIRECT_ACTION_PATTERNS:
        assert_section_forbidden(
            sections,
            "## Mailbox List Select Operation Contract",
            pattern,
        )
    for example in MAILBOX_FORBIDDEN_EXAMPLES:
        assert_any_pattern_matches(
            sections["## Mailbox List Select Operation Contract"] + "\n" + example,
            MAILBOX_FORBIDDEN_SURFACE_PATTERNS
            + MAILBOX_FORBIDDEN_MODAL_PATTERNS
            + MAILBOX_FORBIDDEN_DIRECT_ACTION_PATTERNS,
        )
    for pattern in FETCH_FORBIDDEN_SURFACE_PATTERNS:
        assert_section_forbidden(
            sections,
            "## Message Fetch Operation Contract",
            pattern,
        )
    for pattern in FETCH_FORBIDDEN_MODAL_PATTERNS:
        assert_section_forbidden(
            sections,
            "## Message Fetch Operation Contract",
            pattern,
        )
    for pattern in FETCH_FORBIDDEN_DIRECT_ACTION_PATTERNS:
        assert_section_forbidden(
            sections,
            "## Message Fetch Operation Contract",
            pattern,
        )
    for example in FETCH_FORBIDDEN_EXAMPLES:
        assert_any_pattern_matches(
            sections["## Message Fetch Operation Contract"] + "\n" + example,
            FETCH_FORBIDDEN_SURFACE_PATTERNS
            + FETCH_FORBIDDEN_MODAL_PATTERNS
            + FETCH_FORBIDDEN_DIRECT_ACTION_PATTERNS,
        )


if __name__ == "__main__":
    main()
