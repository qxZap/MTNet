"""Turns plugins/mt_net_panel.html into plugins/mt_net_panel.inc, so the panel ships inside the dll.

build.bat runs this before compiling mt_net.cpp. A byte list because MSVC caps string literals.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGINS = os.path.join(HERE, "..", "plugins")

html = open(os.path.join(PLUGINS, "mt_net_panel.html"), "rb").read()
with open(os.path.join(PLUGINS, "mt_net_panel.inc"), "w") as out:
    for i in range(0, len(html), 32):
        out.write(",".join(str(b) for b in html[i:i + 32]) + ",\n")
print("panel embedded:", len(html), "bytes")
