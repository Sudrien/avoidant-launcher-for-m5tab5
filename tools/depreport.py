#!/usr/bin/env python3
"""
depreport.py -- depcheck.py's answers for several projects as one
Markdown report, the body of the "Dependency updates" issue
(.github/workflows/deps.yml).

    depreport.py NAME:COMMIT:EXIT:FILE.json [...] > report.md

NAME is the repository, COMMIT the commit checked, EXIT depcheck.py's
exit status and FILE its --json output. The first line of the report is
for the workflow, which drops it before posting: "updates" if anything
is newer; "unknown" if nothing newer was found but something could not
be checked -- the registry down, say -- so the issue is left alone
rather than closed on an answer that was not there; "none" otherwise.

Nothing in the body changes unless an answer does -- no date, no run
number -- so a week with nothing new leaves the issue as it was.

Standard library only.

SPDX-License-Identifier: MIT
"""
import json
import sys

NEWER = ("in range", "outside", "WARN")
WHAT = {
    "in range": "newer, and allowed: the next `idf.py update-dependencies` takes it",
    "outside": "newer, but a range rules it out",
    "WARN": "pinned to a tag, and newer tags exist",
    "?": "could not be checked",
}
EXIT = {0: "up to date", 1: "updates", 2: "could not read the project",
        3: "nothing newer found, but not everything could be checked"}


def main(args):
    any_newer = any_unknown = False
    out = []
    for arg in args:
        name, commit, code, path = arg.split(":", 3)
        code = int(code)
        try:
            with open(path, encoding="utf-8") as f:
                rows = json.load(f)
        except (OSError, ValueError):
            rows = []
        newer = [r for r in rows if r["status"] in NEWER]
        unknown = [r for r in rows if r["status"] == "?"]
        any_newer = any_newer or bool(newer)
        any_unknown = any_unknown or bool(unknown) or code in (2, 3) or not rows
        out.append("## [%s](https://github.com/Sudrien/%s/tree/%s)\n" % (name, name, commit))
        out.append("At `%s`: %s.\n" % (commit[:7], EXIT.get(code, "exit %d" % code)))
        if newer:
            out.append("| component | locked | newest | | |")
            out.append("|---|---|---|---|---|")
            for r in newer:
                out.append("| %s | %s | %s | **%s** | %s |" % (
                    r["name"], r["locked"], r["newest"] or "-", r["status"],
                    (r["note"] or "").replace("|", "\\|")))
            out.append("")
        if unknown:
            # One line, names only: an error's text differs run to run,
            # and twenty of them would bury the table.
            out.append("Not checked -- the registry or a remote did not answer: %s.\n"
                       % ", ".join(r["name"] for r in unknown))
    head = "updates" if any_newer else "unknown" if any_unknown else "none"
    key = "\n".join("- **%s**: %s" % (k, v) for k, v in WHAT.items())
    print(head)
    print("Checked weekly by `tools/depcheck.py` in "
          "[deps.yml](https://github.com/Sudrien/avoidant-launcher-for-m5tab5/blob/main/.github/workflows/deps.yml), "
          "against the registry, the git remotes and ESP-IDF's tags. "
          "Only what is newer, or could not be checked, is listed; the run's log has the rest.\n")
    print("\n".join(out))
    print(key)
    print("\nA re-resolved lock is read before it is committed (each project's CLAUDE.md); "
          "this issue only says what exists.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
