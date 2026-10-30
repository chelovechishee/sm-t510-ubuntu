#!/usr/bin/env python3
"""Turn an Android mixer_paths.xml into `amixer -c0 cset` commands.

usage: mixer_paths2amixer.py mixer_paths.xml [--init] [path ...]
  --init   the top-level <ctl> defaults (applied once at audio HAL start)
  path     named <path> elements, nested <path name=.../> references expanded
Controls that the running card does not have are emitted commented out when
--known FILE (one control name per line) is given.
"""
import shlex
import sys
import xml.etree.ElementTree as ET


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    root = ET.parse(args.pop(0)).getroot()
    known = None
    if "--known" in args:
        i = args.index("--known")
        with open(args[i + 1]) as f:
            known = {line.strip() for line in f if line.strip()}
        del args[i:i + 2]

    paths = {p.get("name"): p for p in root.findall("path")}
    out = []

    def emit(ctl):
        name, value = ctl.get("name"), ctl.get("value")
        if value is None:
            return
        cmd = "amixer -q -c0 cset name=%s %s" % (shlex.quote(name), shlex.quote(value))
        if known is not None and name not in known:
            cmd = "# missing: " + cmd
        out.append(cmd)

    def expand(path):
        for el in path:
            if el.tag == "ctl":
                emit(el)
            elif el.tag == "path":
                expand(paths[el.get("name")])

    for name in args:
        if name == "--init":
            for el in root.findall("ctl"):
                emit(el)
        else:
            out.append("# path " + name)
            expand(paths[name])
    print("\n".join(out))


if __name__ == "__main__":
    main()
