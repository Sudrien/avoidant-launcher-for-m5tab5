#!/usr/bin/env python3
"""
depcheck.py -- what has a newer version than an ESP-IDF project's
dependencies.lock, without ESP-IDF.

    python3 depcheck.py [PROJECT_DIR] [--lock FILE] [--debug] [--no-idf] [--json]

PROJECT_DIR is where dependencies.lock is (default: the current
directory; for unprofitable-mahjong, m5tab5/). With no dependencies.lock
there, the one dependencies.lock.* is used -- defeatist keeps only
dependencies.lock.idf6 -- or --lock names it. Every idf_component.yml
under it is read for the ranges you asked for.

For each component in the lock:

  registry   ("type: service")  the versions the Espressif component
             registry publishes, from its web API
  git        ("type: git")      the remote's tags, by `git ls-remote`
  idf                           ESP-IDF's own release tags, the same way
                                (--no-idf to skip: it is the slow one)

and says, for the newest version found:

  ok         the lock has the newest
  in range   newer, and every range that asks for it allows it: the next
             `idf.py update-dependencies` would take it
  outside    newer, but a range rules it out: a manifest change to take
  WARN       a git component pinned to a tag, and a newer tag exists:
             said again on stderr after the table, with how far behind
  ?          could not be checked (the line says why)

The registry's answer is not the component manager's resolver: it says
what exists, not what a resolve would choose with every constraint at
once. "in range" means every range in the lock and the manifests that
names the component allows it, which is usually the same thing.

Standard library only. Exit status 0 with everything checked and
nothing newer, 1 with something newer, 2 on a failure to read the
project, 3 with nothing newer found but something not checked (the
registry unreachable, say) -- so a quiet run is not taken for an
up-to-date one.

SPDX-License-Identifier: MIT
"""
import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

REGISTRY = "https://components.espressif.com"
IDF_GIT = "https://github.com/espressif/esp-idf.git"
TIMEOUT_S = 20


# ---- versions and ranges ----

VER_RE = re.compile(r"^v?(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:[-~](.+))?$")


def parse_ver(s):
    """'1.8.2' -> (1, 8, 2, pre) or None. A prerelease sorts before its release."""
    m = VER_RE.match(str(s).strip())
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2) or 0), int(m.group(3) or 0), m.group(4) or "")


def ver_key(v):
    # a release (pre == "") after any prerelease of the same number
    return (v[0], v[1], v[2], 1 if not v[3] else 0, v[3])


def fmt_ver(v):
    s = "%d.%d.%d" % v[:3]
    return s + ("-" + v[3] if v[3] else "")


def one_clause_ok(clause, v):
    """One clause of a range: ^1.2.3, ~1.2.3, 1.*, *, >=1.2, <2, ==1.2.3, 1.2.3."""
    c = clause.strip().strip("'\"")
    if c in ("", "*"):
        return True
    m = re.match(r"^(\^|~=|~|>=|<=|>|<|==|!=|=)?\s*(.+)$", c)
    op, rest = m.group(1) or "", m.group(2).strip()
    if "*" in rest or rest.lower().endswith(".x"):          # 1.*, 1.2.*
        parts = re.split(r"\.", rest)
        fixed = [p for p in parts if p not in ("*", "x", "X")]
        return list(v[:len(fixed)]) == [int(p) for p in fixed]
    want = parse_ver(rest)
    if not want:
        return True                                          # unreadable: do not rule out
    k, w = ver_key(v), ver_key(want)
    if op == "^":                                            # same leftmost non-zero
        if want[0] > 0:
            return v[0] == want[0] and k >= w
        if want[1] > 0:
            return v[0] == 0 and v[1] == want[1] and k >= w
        return v[:3] == want[:3]
    if op in ("~", "~="):                                    # same major.minor
        return v[0] == want[0] and v[1] == want[1] and k >= w
    if op == ">=":
        return k >= w
    if op == "<=":
        return k <= w
    if op == ">":
        return k > w
    if op == "<":
        return k < w
    if op == "!=":
        return v[:3] != want[:3]
    return v[:3] == want[:3]                                 # ==, = or bare


def range_ok(rng, v):
    """A range with commas (all) and || (any)."""
    return any(all(one_clause_ok(c, v) for c in alt.split(","))
               for alt in str(rng).split("||"))


# ---- the lock and the manifests: just enough YAML ----

def read_lock(path):
    """{name: {"version", "type", "git", "requires": [(by, range)]}} from dependencies.lock."""
    comps, cur, in_deps, in_sub, sub = {}, None, False, False, None

    def flush(by, item):
        if item and item.get("name") and item.get("version"):
            comps.setdefault("_req", {}).setdefault(item["name"], []).append((by, item["version"]))
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line == "dependencies:":
                in_deps = True
                continue
            if re.match(r"^\S", line):                        # direct_dependencies, target, ...
                if in_sub:
                    flush(cur, sub)
                in_deps, in_sub, sub = False, False, None
                continue
            if not in_deps:
                continue
            m = re.match(r"^  (\S[^:]*):\s*$", line)
            if m:
                if in_sub:
                    flush(cur, sub)
                in_sub, sub = False, None
                cur = m.group(1)
                comps[cur] = {"version": None, "type": None, "git": None, "requires": []}
                in_sub = False
                continue
            if cur is None:
                continue
            if re.match(r"^    dependencies:\s*$", line):
                in_sub, sub = True, None
                continue
            m = re.match(r"^    (\w+):\s*(.*)$", line)
            if m:                                             # back at the component's level
                if in_sub:
                    flush(cur, sub)
                    sub = None
                in_sub = False
                if m.group(1) == "version":
                    comps[cur]["version"] = m.group(2).strip().strip("'\"")
                continue
            if in_sub:
                # A list item's keys come in any order -- "- git:" can lead,
                # with name: under it -- so each item is gathered whole
                # and recorded when the next one starts or the list ends.
                m = re.match(r"^    - (\w+):\s*(.*)$", line)
                if m:
                    flush(cur, sub)
                    sub = {m.group(1): m.group(2).strip().strip("'\"")}
                    continue
                m = re.match(r"^      (\w+):\s*(.*)$", line)
                if m and sub is not None:
                    sub[m.group(1)] = m.group(2).strip().strip("'\"")
                continue
            m = re.match(r"^      (type|git):\s*(.+)$", line)
            if m:
                comps[cur][m.group(1)] = m.group(2).strip()
    reqs = comps.pop("_req", {})
    for name, rs in reqs.items():
        if name in comps:
            comps[name]["requires"].extend(rs)
    return comps


def read_manifests(root):
    """[(manifest path, name, range or git tag)] from every idf_component.yml under root."""
    out = []
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ("build", "managed_components", ".git")]
        if "idf_component.yml" not in files:
            continue
        path = os.path.join(d, "idf_component.yml")
        name, in_deps = None, False
        with open(path, encoding="utf-8") as f:
            for raw in f:
                line = raw.split("#", 1)[0].rstrip()
                if not line.strip():
                    continue
                if re.match(r"^dependencies:\s*$", line):
                    in_deps = True
                    continue
                if re.match(r"^\S", line):
                    in_deps = False
                    continue
                if not in_deps:
                    continue
                m = re.match(r"^  (\S[^:]*):\s*(.*)$", line)
                if m:
                    name = m.group(1).strip().strip("'\"")
                    val = m.group(2).strip().strip("'\"")
                    if val:
                        out.append((path, name, val))
                    continue
                m = re.match(r"^    version:\s*(.+)$", line)
                if m and name:
                    out.append((path, name, m.group(1).strip().strip("'\"")))
    return out


# ---- what exists ----

def registry_versions(name, debug):
    """The registry's published versions of namespace/name, newest first."""
    ns, _, comp = name.partition("/")
    url = "%s/api/components/%s/%s" % (REGISTRY, ns, comp)
    req = urllib.request.Request(url, headers={"Accept": "application/json",
                                               "User-Agent": "depcheck.py"})
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
        data = json.load(r)
    if debug:
        print("  [debug] %s: top-level keys %s" % (url, sorted(data) if isinstance(data, dict) else type(data).__name__))
    found = []
    vers = data.get("versions") if isinstance(data, dict) else None
    for item in vers or []:
        s = item.get("version") if isinstance(item, dict) else item
        yanked = isinstance(item, dict) and (item.get("yanked_at") or item.get("yanked"))
        v = parse_ver(s) if s else None
        if v and not yanked:
            found.append(v)
    if not found:
        raise ValueError("no versions in the answer (try --debug for its shape)")
    return sorted(set(found), key=ver_key, reverse=True)


def git_tags(url):
    """{tag: {shas}} for the remote's tags: the tag object's and, for an
    annotated tag, the commit it points at. dependencies.lock records
    the first -- for feckless_drivers v0.5.0, b56e1f0, the tag object,
    not c7e943e, its commit -- so a match on either is a match."""
    out = subprocess.run(["git", "ls-remote", "--tags", url], capture_output=True,
                         text=True, timeout=TIMEOUT_S * 3, check=True).stdout
    tags = {}
    for line in out.splitlines():
        sha, _, ref = line.partition("\t")
        if not ref.startswith("refs/tags/"):
            continue
        tag = ref[len("refs/tags/"):]
        if tag.endswith("^{}"):
            tag = tag[:-3]                        # the commit an annotated tag points at
        tags.setdefault(tag, set()).add(sha)
    return tags


def newest_tag(tags, releases_only=True):
    best = None
    for t in tags:
        v = parse_ver(t)
        if not v or (releases_only and v[3]):
            continue
        if best is None or ver_key(v) > ver_key(best[1]):
            best = (t, v)
    return best


# ---- the report ----

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("project", nargs="?", default=".")
    ap.add_argument("--lock", help="the lock file, if not dependencies.lock")
    ap.add_argument("--debug", action="store_true", help="show what the registry answered")
    ap.add_argument("--no-idf", action="store_true", help="do not look for ESP-IDF releases")
    ap.add_argument("--json", action="store_true", help="the rows as JSON, for a workflow")
    a = ap.parse_args()

    lock = a.lock or os.path.join(a.project, "dependencies.lock")
    if not a.lock and not os.path.exists(lock):
        others = sorted(f for f in os.listdir(a.project) if f.startswith("dependencies.lock."))
        if len(others) == 1:
            lock = os.path.join(a.project, others[0])
        elif others:
            print("several locks in %s (%s): name one with --lock" % (a.project, ", ".join(others)),
                  file=sys.stderr)
            return 2
    if not os.path.exists(lock):
        print("no dependencies.lock in %s" % a.project, file=sys.stderr)
        return 2
    print("%s" % os.path.relpath(lock), file=sys.stderr)
    comps = read_lock(lock)
    manifests = read_manifests(a.project)
    asked = {}
    for path, name, rng in manifests:
        asked.setdefault(name, []).append((os.path.relpath(path, a.project), rng))

    rows = []
    for name in sorted(comps):
        c = comps[name]
        row = {"name": name, "type": c["type"], "locked": c["version"], "newest": None,
               "status": "?", "note": ""}
        try:
            if c["type"] == "service":
                have = parse_ver(c["version"])
                vers = registry_versions(name, a.debug)
                releases = [v for v in vers if not v[3]] or vers
                top = releases[0]
                row["newest"] = fmt_ver(top)
                ranges = [(by, r) for by, r in c["requires"]] + \
                         [(m, r) for m, r in asked.get(name, [])]
                if have and ver_key(top) <= ver_key(have):
                    row["status"] = "ok"
                else:
                    bad = [(by, r) for by, r in ranges if not range_ok(r, top)]
                    if bad:
                        row["status"] = "outside"
                        row["note"] = "ruled out by " + "; ".join("%s %s" % (by, r) for by, r in bad)
                        allowed = [v for v in releases if all(range_ok(r, v) for _, r in ranges)]
                        if allowed and have and ver_key(allowed[0]) > ver_key(have):
                            row["note"] += "; newest in range is " + fmt_ver(allowed[0])
                    else:
                        row["status"] = "in range"
                        row["note"] = "allowed by " + ("; ".join("%s %s" % (by, r) for by, r in ranges) or "no range")
            elif c["type"] == "git":
                tags = git_tags(c["git"])
                best = newest_tag(tags)
                pinned = [r for _, r in asked.get(name, [])]
                pin = pinned[0] if pinned else None
                at = [t for t, shas in tags.items() if c["version"] in shas]
                row["locked"] = "%s (%s)" % (c["version"][:7], ", ".join(sorted(at)) or "no tag")
                if not best:
                    row["status"], row["note"] = "?", "no release tags on the remote"
                else:
                    row["newest"] = best[0]
                    pv = parse_ver(pin) if pin else None
                    if pin and pin in tags and c["version"] not in tags[pin]:
                        row["note"] = "the lock is not at %s's commit; a build would re-resolve" % pin
                    if pv and ver_key(best[1]) > ver_key(pv):
                        behind = sorted((t for t in tags if parse_ver(t) and not parse_ver(t)[3]
                                         and ver_key(parse_ver(t)) > ver_key(pv)),
                                        key=lambda t: ver_key(parse_ver(t)))
                        row["status"] = "WARN"
                        row["note"] = ("pinned to %s, %d newer tag%s: %s" %
                                       (pin, len(behind), "" if len(behind) == 1 else "s",
                                        ", ".join(behind))) + ("; " + row["note"] if row["note"] else "")
                        row["warn"] = "%s is pinned to %s in %s; %s is the newest tag" % (
                            name, pin, ", ".join(m for m, _ in asked.get(name, [])), best[0])
                    elif not pv and best[0] not in at:
                        row["status"] = "in range"
                        row["note"] = "manifest names %s, not a tag" % (pin or "nothing")
                    else:
                        row["status"] = row["status"] if row["note"] else "ok"
                        if row["note"]:
                            row["status"] = "?"
            elif c["type"] == "idf":
                if a.no_idf:
                    row["status"], row["note"] = "skipped", "--no-idf"
                else:
                    have = parse_ver(c["version"])
                    tags = git_tags(IDF_GIT)
                    same_minor = [parse_ver(t) for t in tags if parse_ver(t) and not parse_ver(t)[3]
                                  and have and parse_ver(t)[:2] == have[:2]]
                    best = newest_tag(tags)
                    row["newest"] = best[0] if best else None
                    patch = max(same_minor, key=ver_key) if same_minor else None
                    if best and have and ver_key(best[1]) > ver_key(have):
                        row["status"] = "outside"
                        row["note"] = "CI picks the IDF (idf_version in the workflow)"
                        if patch and ver_key(patch) > ver_key(have):
                            row["note"] += "; newest %d.%d is %s" % (have[0], have[1], fmt_ver(patch))
                    else:
                        row["status"] = "ok"
            elif c["type"] == "local":
                row["status"], row["note"] = "skipped", "a local component"
            else:
                row["note"] = "unknown source type %r" % c["type"]
        except (urllib.error.URLError, OSError, ValueError, subprocess.SubprocessError, json.JSONDecodeError) as e:
            row["status"], row["note"] = "?", "%s: %s" % (type(e).__name__, e)
        rows.append(row)

    if a.json:
        json.dump(rows, sys.stdout, indent=2)
        print()
    else:
        w = max(len(r["name"]) for r in rows)
        lw = max(len(str(r["locked"])) for r in rows)
        for r in rows:
            print("%-*s  %-*s  %-10s  %-8s  %s" % (w, r["name"], lw, r["locked"],
                                                  r["newest"] or "-", r["status"], r["note"]))
    count = lambda *st: sum(r["status"] in st for r in rows)
    for r in rows:
        if r.get("warn"):
            print("warning: " + r["warn"], file=sys.stderr)
    newer, unknown = count("in range", "outside", "WARN"), count("?")
    print("%d newer, %d not checked, %d up to date" % (newer, unknown, count("ok")), file=sys.stderr)
    if unknown and not newer:
        return 3
    return 1 if newer else 0


if __name__ == "__main__":
    sys.exit(main())
