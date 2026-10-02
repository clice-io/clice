"""Prints the time of a link action in a Bazel JSON profile: the action, the
linker process, and each sandbox step around it.

usage: lto-profile.py <profile.json> <target>
"""

import json
import sys

path, target = sys.argv[1], sys.argv[2]
try:
    with open(path) as f:
        data = json.load(f)
except (OSError, ValueError):
    print("link=-")
    sys.exit()
events = data["traceEvents"] if isinstance(data, dict) else data
links = [
    e
    for e in events
    if e.get("ph") == "X"
    and e.get("cat") == "action processing"
    and e.get("name", "").split(" ")[:2] in (["Linking", target], ["Linking", target + ".exe"])
]
if not links:
    print("link=-")
    sys.exit()
link = max(links, key=lambda e: e["dur"])
start, end = link["ts"], link["ts"] + link["dur"]
steps = {}
for e in events:
    if e.get("ph") != "X" or e.get("tid") != link.get("tid") or not start <= e["ts"] <= end:
        continue
    name = e.get("name", "")
    if name.startswith("sandbox.") or name == "subprocess.run":
        steps[name] = steps.get(name, 0) + e["dur"]
run = steps.pop("subprocess.run", 0)
detail = ",".join("%s=%.0fms" % (k.removeprefix("sandbox."), v / 1e3) for k, v in sorted(steps.items()))
print("link=%.1fs\tprocess=%.1fs\tsandbox[%s]" % (link["dur"] / 1e6, run / 1e6, detail))
