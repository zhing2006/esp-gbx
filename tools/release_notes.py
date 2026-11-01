#!/usr/bin/env python3
"""Print the release notes of one version from CHANGELOG.md.

    tools/release_notes.py v0.1.0 [--changelog CHANGELOG.md]

Each version in CHANGELOG.md is a second-level heading whose first word is
the version's tag, such as "## v0.1.0". The notes are the section's body, up
to the next second-level heading, without the blank lines around it. Headings
inside fenced code blocks do not count.

The release workflow, .github/workflows/release.yml, runs this with the tag it
publishes. It exits with 1, and the release stops, when the version has no
section or the section is empty.
"""

import argparse
import sys
from pathlib import Path


def section(text: str, tag: str) -> list[str] | None:
    lines: list[str] | None = None
    fenced = False
    for line in text.splitlines():
        if line.lstrip().startswith(("```", "~~~")):
            fenced = not fenced
        heading = not fenced and line.startswith("## ")
        if lines is not None:
            if heading:
                break
            lines.append(line)
        elif heading and line[3:].split(maxsplit=1)[:1] == [tag]:
            lines = []
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tag", help="version tag, such as v0.1.0")
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    args = parser.parse_args()

    lines = section(args.changelog.read_text(encoding="utf-8"), args.tag)
    if lines is None:
        print(f"{args.changelog}: no section for {args.tag}", file=sys.stderr)
        return 1
    notes = "\n".join(lines).strip("\n")
    if not notes.strip():
        print(f"{args.changelog}: the section for {args.tag} is empty", file=sys.stderr)
        return 1
    sys.stdout.write(notes + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
