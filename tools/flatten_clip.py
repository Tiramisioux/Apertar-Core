#!/usr/bin/env python3
"""Flatten (or, for tests, split) an Apertar cDNG clip folder.

Usage:
    python3 -I tools/flatten_clip.py <clip folder> [--apply] [--unflatten N]

    (no flag)      dry run: print what would be done, change nothing (--dry-run is the same, explicitly)
    --apply        do it
    --unflatten N  the inverse, for tests: move the root's frames into part_<k>/ (k = frame // N,
                   100 <= N <= 100000) and write the .parts marker

Why: with [storage] frames_per_folder=N the camera stores frame f of a take in <clip>/part_<k>/ (k = f // N).
DaVinci Resolve and Premiere import one folder as one DNG sequence, so a split clip must be flattened
(every part_*/*.dng moved back into the clip root, names unchanged) before import.

Cost: on exFAT every create or rename into a folder walks all entries of the target folder under the
filesystem lock, so merging 45,000 files into one folder is O(N^2) (measured ~0.32 us per existing file
per rename). The moves are therefore done oldest part first, in frame order: at every moment the clip is
"a valid flat prefix in the root + intact remaining parts", so a power cut leaves a state this script
resumes by simply running it again. The .parts marker is removed last.

Exit codes: 0 ok / nothing to do, 1 refused (reason on stderr), 2 usage error.
Standard library only; Python 3.9+; macOS and Raspberry Pi OS.
"""
import os
import re
import statistics
import sys
import time

MIN_N, MAX_N = 100, 100000
PART_PREFIX = "part_"
MARKER = ".parts"
FSYNC_EVERY = 1000


def is_part_dir_name(name):
    # Same rule as FramePath.hpp isPartDirName: "part_" + at least 3 digits, nothing else.
    return (len(name) >= len(PART_PREFIX) + 3 and name.startswith(PART_PREFIX)
            and name[len(PART_PREFIX):].isascii() and name[len(PART_PREFIX):].isdigit())


def is_dng(name):
    return name.lower().endswith(".dng")


def frame_number(name):
    """'<stem>_<digits>.dng' -> int (digits right before the last '.', after '_', at most 18); else None."""
    dot = name.rfind(".")
    if dot <= 0:
        return None
    i = dot
    while i > 0 and name[i - 1] in "0123456789":
        i -= 1
    if i == dot or i == 0 or name[i - 1] != "_" or dot - i > 18:
        return None
    return int(name[i:dot])


def sort_key(name):
    n = frame_number(name)
    return (0, n, name) if n is not None else (1, 0, name)


def refuse(msg):
    print("refused: " + msg, file=sys.stderr)
    return 1


def fsync_dir(path):
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass  # macOS and some filesystems refuse fsync on a directory fd
    finally:
        os.close(fd)


def scan(root):
    """(root files, {part dir name: [file names]}, part non-dng counts). Regular files only."""
    root_files, parts = [], {}
    with os.scandir(root) as it:
        for e in it:
            if e.is_dir(follow_symlinks=False) and is_part_dir_name(e.name):
                with os.scandir(e.path) as pit:
                    parts[e.name] = sorted(p.name for p in pit if p.is_file(follow_symlinks=False))
            elif e.is_file(follow_symlinks=False):
                root_files.append(e.name)
    return sorted(root_files), parts


def part_number(dirname):
    return int(dirname[len(PART_PREFIX):])


def pct(sorted_vals, p):
    return sorted_vals[min(len(sorted_vals) - 1, int(len(sorted_vals) * p))]


def flatten(root, apply):
    root_files, parts = scan(root)
    has_marker = MARKER in root_files
    root_frames = [n for n in root_files if is_dng(n)]
    part_names = sorted(parts, key=part_number)
    part_dng = {p: sorted((n for n in parts[p] if is_dng(n)), key=sort_key) for p in part_names}
    part_other = {p: [n for n in parts[p] if not is_dng(n)] for p in part_names}
    total_in_parts = sum(len(v) for v in part_dng.values())
    other_root = len(root_files) - len(root_frames) - (1 if has_marker else 0)

    if not parts and not has_marker:
        print("already flat")
        return 0

    print("before: %d frames in root, %d part folders, %d frames in parts, %d other files in root%s, %d other files in parts"
          % (len(root_frames), len(parts), total_in_parts, other_root, " (+ .parts marker)" if has_marker else "",
             sum(len(v) for v in part_other.values())))

    # Plan and check, before anything moves.
    steps = [(p, n) for p in part_names for n in part_dng[p]]
    taken = {n.casefold() for n in root_files}
    seen, conflicts = {}, []
    for p, n in steps:
        k = n.casefold()
        if k in taken:
            conflicts.append("%s/%s: %s already exists in the clip root" % (p, n, n))
        elif k in seen:
            conflicts.append("%s/%s: same name as %s/%s" % (p, n, seen[k], n))
        else:
            seen[k] = p
    if conflicts:
        print("%d conflict(s), first %d:" % (len(conflicts), min(5, len(conflicts))), file=sys.stderr)
        for c in conflicts[:5]:
            print("  " + c, file=sys.stderr)
        return refuse("nothing moved")

    if not apply:
        print("dry run: would move %d frames from %d part folders into the root (part order, then frame order)"
              % (len(steps), len(parts)))
        for p in part_names:
            note = "" if not part_other[p] else " (keeps %d non-DNG file(s), folder stays)" % len(part_other[p])
            print("  %s: %d frames%s" % (p, len(part_dng[p]), note))
        if has_marker:
            print("would remove %s last%s" % (MARKER, "" if not any(part_other.values()) else " (only if no part folder is left)"))
        print("dry run: nothing changed (use --apply)")
        return 0

    times = []
    left = []
    t0 = time.perf_counter()
    count = 0
    for p in part_names:
        for n in part_dng[p]:
            a = time.perf_counter_ns()
            os.rename(os.path.join(root, p, n), os.path.join(root, n))
            times.append(time.perf_counter_ns() - a)
            count += 1
            if count % FSYNC_EVERY == 0:
                fsync_dir(root)
        try:
            os.rmdir(os.path.join(root, p))
        except OSError:
            left.append(p)
            print("%s not empty (%d other file(s): %s), left in place"
                  % (p, len(part_other[p]), ", ".join(part_other[p][:3]) or "?"))
    fsync_dir(root)
    elapsed = time.perf_counter() - t0

    if has_marker:
        if left:
            print("%s kept: %d part folder(s) remain" % (MARKER, len(left)))
        else:
            os.unlink(os.path.join(root, MARKER))
            fsync_dir(root)
    root_files2, parts2 = scan(root)
    print("after: %d frames in root, %d part folders remain%s"
          % (sum(1 for n in root_files2 if is_dng(n)), len(parts2), "" if MARKER in root_files2 else ", no .parts"))
    if times:
        ms = sorted(t / 1e6 for t in times)
        print("moved %d frames in %.2f s; rename median %.3f ms, p90 %.3f ms"
              % (len(times), elapsed, statistics.median(ms), pct(ms, 0.9)))
        if len(times) > 3000:
            line = []
            for i in range(0, len(times), 1000):
                chunk = [t / 1e6 for t in times[i:i + 1000]]
                line.append("%d-%d: %.3f" % (i, i + len(chunk), statistics.median(chunk)))
            print("median ms per 1000 renames (in move order):")
            for l in line:
                print("  " + l)
    return 0


def unflatten(root, n, apply):
    if not (MIN_N <= n <= MAX_N):
        return refuse("--unflatten N must be from %d to %d" % (MIN_N, MAX_N))
    root_files, parts = scan(root)
    if parts:
        return refuse("the clip already has part folders (%d); unflatten needs a flat clip" % len(parts))
    if MARKER in root_files:
        return refuse("%s already exists; unflatten needs a flat clip" % MARKER)
    frames = sorted((x for x in root_files if is_dng(x)), key=sort_key)
    if not frames:
        return refuse("no .dng frames in the clip root")
    bad = [x for x in frames if frame_number(x) is None]
    if bad:
        return refuse("%d frame file(s) carry no frame number, e.g. %s" % (len(bad), bad[0]))

    def prefix_of(name):
        i = name.rfind(".")
        while name[i - 1] in "0123456789":
            i -= 1
        return name[:i]

    first = frames[0]
    prefix = prefix_of(first)
    digits = len(first[len(prefix):first.rfind(".")])
    mixed = any(prefix_of(x) != prefix for x in frames)
    pattern = "%s%%0%dd.dng" % (prefix, digits)
    print("before: %d frames in root; splitting by frame // %d" % (len(frames), n))
    if mixed:
        print("note: the frame names do not share one prefix; pattern uses the first name's prefix '%s'" % prefix)
    per = {}
    for x in frames:
        per.setdefault(frame_number(x) // n, []).append(x)
    for k in sorted(per):
        d = PART_PREFIX + str(k).zfill(3)
        print("  %s: %d frames" % (d, len(per[k])))
        if not apply:
            continue
        os.mkdir(os.path.join(root, d))
        for x in per[k]:
            os.rename(os.path.join(root, x), os.path.join(root, d, x))
        fsync_dir(root)
    marker = "frames_per_folder=%d\npattern=%s\n" % (n, pattern)
    if not apply:
        print("dry run: would write %s:\n%s" % (MARKER, marker.rstrip("\n")))
        print("dry run: nothing changed (use --apply)")
        return 0
    with open(os.path.join(root, MARKER), "w") as f:
        f.write(marker)
        f.flush()
        os.fsync(f.fileno())
    fsync_dir(root)
    print("after: %d part folders, wrote %s" % (len(per), MARKER))
    return 0


def main(argv):
    usage = "usage: python3 -I tools/flatten_clip.py <clip folder> [--apply] [--dry-run] [--unflatten N]"
    path, apply, n, i = None, False, None, 0
    while i < len(argv):
        a = argv[i]
        if a == "--apply":
            apply = True
        elif a == "--dry-run":
            pass
        elif a == "--unflatten":
            i += 1
            if i >= len(argv) or not re.fullmatch(r"[0-9]+", argv[i]):
                print(usage, file=sys.stderr)
                return 2
            n = int(argv[i])
        elif a.startswith("-") or path is not None:
            print(usage, file=sys.stderr)
            return 2
        else:
            path = a
        i += 1
    if path is None:
        print(usage, file=sys.stderr)
        return 2
    if not os.path.isdir(path):
        return refuse("%s is not a directory" % path)
    try:
        return unflatten(path, n, apply) if n is not None else flatten(path, apply)
    except OSError as e:
        return refuse("%s (stopped; run again to resume)" % e)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
