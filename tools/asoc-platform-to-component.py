#!/usr/bin/env python3
"""Convert a 4.4-era ASoC "platform" driver (snd_soc_platform_driver with a
snd_pcm_ops table) to a snd_soc_component_driver, in place.

usage: asoc-platform-to-component.py FILE...

Handles what the Samsung ABOX RDMA/WDMA drivers use: the pcm ops get the
component as first argument, rtd->platform/cpu_dai/codec become component /
snd_soc_rtd_to_cpu() / snd_soc_rtd_to_codec(), pcm_new/pcm_free take the component too,
and the platform (un)registration becomes
devm_snd_soc_register_component(). Anything else is left for review.
"""
import re
import sys

OPS = ["open", "close", "hw_params", "hw_free", "prepare", "trigger", "pointer",
       "mmap", "copy", "ack"]


def convert(s):
    # pcm ops table -> callbacks map
    m = re.search(r"static (?:const )?struct snd_pcm_ops (\w+) = \{(.*?)\n\};\n", s, re.S)
    if not m:
        raise SystemExit("no snd_pcm_ops table")
    ops_name, body = m.group(1), m.group(2)
    ops = dict(re.findall(r"\.(\w+)\s*=\s*(\w+),", body))
    s = s[:m.start()] + s[m.end():]

    for op, fn in ops.items():
        if op not in OPS:
            continue
        s = re.sub(r"(static (?:int|snd_pcm_uframes_t) %s\()" % fn,
                   r"\1struct snd_soc_component *component,\n\t\t", s)

    # runtime accessors
    s = re.sub(r"struct snd_soc_platform \*platform = (?:rtd|runtime)->platform;",
               "struct snd_soc_component *platform = component;", s)
    s = re.sub(r"(\w+)->cpu_dai\b", r"snd_soc_rtd_to_cpu(\1, 0)", s)
    s = re.sub(r"(\w+)->codec->dev\b", r"snd_soc_rtd_to_codec(\1, 0)->dev", s)
    s = s.replace("snd_soc_platform_get_drvdata(", "snd_soc_component_get_drvdata(")

    # platform driver -> component driver
    m = re.search(r"(?:static )?(?:const )?struct snd_soc_platform_driver (\w+) = \{(.*?)\n\};\n", s, re.S)
    if not m:
        raise SystemExit("no snd_soc_platform_driver")
    drv, body = m.group(1), m.group(2)
    fields = dict(re.findall(r"\.(\w+)\s*=\s*&?(\w+),", body))
    lines = ["const struct snd_soc_component_driver %s = {" % drv
             if not m.group(0).startswith("static") else
             "static const struct snd_soc_component_driver %s = {" % drv,
             '\t.name\t\t= "%s",' % drv.replace("_", "-")]
    for op in OPS:
        if op in ops:
            lines.append("\t.%s\t= %s," % (op if op != "hw_params" else "hw_params", ops[op]))
    for cb in ("probe", "remove"):
        if cb in fields:
            lines.append("\t.%s\t\t= %s," % (cb, fields[cb]))
            s = re.sub(r"(static (?:int|void) %s\()struct snd_soc_platform \*platform\)" % fields[cb],
                       r"\1struct snd_soc_component *platform)", s)
    if "pcm_new" in fields:
        lines.append("\t.pcm_new\t= %s," % fields["pcm_new"])
    if "pcm_free" in fields:
        lines.append("\t.pcm_free\t= %s," % fields["pcm_free"])
    lines.append("};\n")
    s = s[:m.start()] + "\n".join(lines) + s[m.end():]

    if "pcm_new" in fields:
        s = re.sub(r"static int %s\(struct snd_soc_pcm_runtime \*runtime\)" % fields["pcm_new"],
                   "static int %s(struct snd_soc_component *component,\n\t\tstruct snd_soc_pcm_runtime *runtime)"
                   % fields["pcm_new"], s)
    if "pcm_free" in fields:
        s = re.sub(r"static void %s\(struct snd_pcm \*pcm\)" % fields["pcm_free"],
                   "static void %s(struct snd_soc_component *component,\n\t\tstruct snd_pcm *pcm)"
                   % fields["pcm_free"], s)

    s = re.sub(r"return snd_soc_register_platform\(([^,]+), &(\w+)\);",
               r"return devm_snd_soc_register_component(\1, &\2, NULL, 0);", s)
    s = re.sub(r"\n\tsnd_soc_unregister_platform\([^)]*\);", "", s)
    return s


for path in sys.argv[1:]:
    with open(path) as f:
        src = f.read()
    out = convert(src)  # convert first: a failure must not truncate the file
    with open(path, "w") as f:
        f.write(out)
