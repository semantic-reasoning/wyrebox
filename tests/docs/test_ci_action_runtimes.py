#!/usr/bin/env python3

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

# First major versions of each action that run on Node.js 24; older majors
# target the deprecated Node.js 20 runtime.
MINIMUM_NODE24_MAJOR = {
    "actions/cache": 5,
    "actions/checkout": 5,
}
USES = re.compile(r"^\s+uses:\s*([\w.-]+/[\w.-]+)@v(\d+)", re.M)


def main() -> None:
    checked = 0
    for workflow in sorted(WORKFLOWS_DIR.glob("*.yml")):
        text = workflow.read_text(encoding="utf-8")
        for action, major in USES.findall(text):
            minimum = MINIMUM_NODE24_MAJOR.get(action)
            if minimum is None:
                continue
            assert int(major) >= minimum, (
                f"{workflow.name}: {action}@v{major} targets Node.js 20; "
                f"use v{minimum} or newer"
            )
            checked += 1
    assert checked, "no pinned actions found to check"
    print(f"node24 action runtimes checked for {checked} uses")


if __name__ == "__main__":
    main()
