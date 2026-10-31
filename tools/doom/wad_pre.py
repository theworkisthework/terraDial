# PlatformIO pre-script for the doom env: rebuilds doom/terrapen.wad from
# tools/doom/make_wad.py before every build, so the embedded WAD always
# matches the generator.
import os
import subprocess

Import("env")  # noqa: F821

root = env["PROJECT_DIR"]  # noqa: F821
os.makedirs(os.path.join(root, "doom"), exist_ok=True)
subprocess.check_call([env.subst("$PYTHONEXE"), os.path.join(root, "tools", "doom", "make_wad.py"),  # noqa: F821
                       os.path.join(root, "doom", "terrapen.wad")])
