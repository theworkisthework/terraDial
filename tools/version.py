# PlatformIO pre-build script: stamp the firmware with a version string.
#
# Priority:
#   1. $TERRADIAL_VERSION      -- what CI sets, from the pushed tag
#   2. `git describe --tags --exact-match` on HEAD -- a local tagged build
#   3. "dev"                   -- anything else
#
# Only an EXACT tag yields a release version. A commit that merely descends
# from a tag is a development build, and stamping it "0.2.0" would make an
# OTA check compare a newer local build against an older published release
# and offer to downgrade it. See include/version.h.
import subprocess, os

Import("env")  # noqa: F821  (injected by PlatformIO)


def _git(*args):
    try:
        return subprocess.check_output(
            ["git"] + list(args), stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return ""


version = os.environ.get("TERRADIAL_VERSION", "").strip()
if not version:
    version = _git("describe", "--tags", "--exact-match")

version = version.lstrip("v") if version else "dev"

# What About shows. A release is just its version. A dev build says where it
# came from instead of a bare "dev": the last release, how many commits past
# it, and the commit -- "0.1.4+28 (d28dfa0)", with a "*" when built from
# uncommitted changes. Display only: OTA still compares FIRMWARE_VERSION.
label = version
if version == "dev":
    desc = _git("describe", "--tags", "--long", "--dirty", "--always")
    dirty = desc.endswith("-dirty")
    if dirty:
        desc = desc[: -len("-dirty")]
    parts = desc.rsplit("-", 2)
    if len(parts) == 3 and parts[2].startswith("g"):
        label = "%s+%s (%s)" % (parts[0].lstrip("v"), parts[1], parts[2][1:])
    elif desc:
        label = "dev (%s)" % desc  # no tag reachable: just the commit
    if dirty:
        label += "*"

print("terraDial firmware version: %s (shown as %s)" % (version, label))
env.Append(CPPDEFINES=[
    ("FIRMWARE_VERSION", env.StringifyMacro(version)),
    ("FIRMWARE_LABEL", env.StringifyMacro(label)),
])
