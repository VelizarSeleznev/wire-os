# Compatibility shim for older WireOS local.conf.
#
# Newer OE-Core no longer ships the user class referenced by this checkout's
# generated local.conf. Keep the class intentionally empty so USER_CLASSES can
# still inherit it without affecting task behavior.
def get_git_latest_tag(path, d):
    import os
    import subprocess

    workspace = d.getVar("WORKSPACE") or os.getcwd()
    candidates = [path, workspace]
    for candidate in candidates:
        if not candidate or not os.path.isdir(candidate):
            continue
        try:
            return subprocess.check_output(
                ["git", "-C", candidate, "describe", "--tags", "--always", "--dirty"],
                stderr=subprocess.DEVNULL,
                text=True,
            ).strip()
        except Exception:
            pass
    return d.getVar("ANKI_BUILD_VERSION") or "0"
