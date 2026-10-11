#!/usr/bin/env python3
"""Merge per-package CMake compilation databases for workspace IntelliSense."""

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {Path(sys.argv[0]).name} <build-directory>", file=sys.stderr)
        return 2

    build_root = Path(sys.argv[1]).resolve()
    entries_by_file = {}
    for database in sorted(build_root.glob("*/compile_commands.json")):
        try:
            entries = json.loads(database.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"Skipping {database}: {error}", file=sys.stderr)
            continue
        for entry in entries:
            source = Path(entry["file"])
            if not source.is_absolute():
                source = Path(entry["directory"]) / source
            entries_by_file[str(source.resolve())] = entry

    output = build_root / "compile_commands.json"
    output.write_text(
        json.dumps(list(entries_by_file.values()), indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"Merged {len(entries_by_file)} compilation commands into {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
