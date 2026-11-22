#!/usr/bin/env python3
"""PlistBuddy operations exercised by the deployment scripts."""
import plistlib
import sys


def convert(kind, value):
    if kind == "dict":
        return {}
    if kind == "array":
        return []
    if kind == "bool":
        if value not in ("true", "false"):
            raise ValueError("expected true or false")
        return value == "true"
    if kind == "integer":
        return int(value)
    if kind == "string":
        return value
    raise ValueError("unsupported type: " + kind)


def main():
    if len(sys.argv) != 4 or sys.argv[1] != "-c":
        raise ValueError('usage: plistbuddy-stub.py -c "COMMAND :path" FILE')
    command, path, *rest = sys.argv[2].split(" ", 2)
    with open(sys.argv[3], "rb") as stream:
        root = plistlib.load(stream)
    parts = path.removeprefix(":").split(":")
    parent = root
    for part in parts[:-1]:
        parent = parent[int(part) if isinstance(parent, list) else part]
    key = int(parts[-1]) if isinstance(parent, list) and parts[-1] else parts[-1]
    value = rest[0] if rest else ""
    if command == "Print":
        item = parent[key]
        print(str(item).lower() if isinstance(item, bool) else item)
        return
    if command == "Set":
        old = parent[key]
        kind = {bool: "bool", int: "integer", str: "string"}[type(old)]
        parent[key] = convert(kind, value)
    elif command == "Delete":
        del parent[key]
    elif command == "Add":
        kind, _, text = value.partition(" ")
        item = convert(kind, text)
        if isinstance(parent, list):
            if key == "":
                parent.append(item)
            else:
                parent.insert(key, item)
        else:
            if key in parent:
                raise ValueError("entry already exists")
            parent[key] = item
    else:
        raise ValueError("unsupported command: " + command)
    with open(sys.argv[3], "wb") as stream:
        plistlib.dump(root, stream, fmt=plistlib.FMT_XML)


if __name__ == "__main__":
    try:
        main()
    except (KeyError, IndexError, ValueError, TypeError, OSError, plistlib.InvalidFileException) as error:
        print("PlistBuddy: " + str(error), file=sys.stderr)
        sys.exit(1)
