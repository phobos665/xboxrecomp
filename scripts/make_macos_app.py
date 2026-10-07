#!/usr/bin/env python3
"""Wrap a title's macOS executable in an application bundle you can double-click.

    python3 scripts/make_macos_app.py titles/timesplitters2/build-mac/timesplitters2_recomp \\
        --name "TimeSplitters 2" [--out <dir>] [--bundle-id <id>] [--game <folder>] \\
        [--no-sign] [--dry-run]

FOR YOUR OWN MACHINE ONLY. A title's .app holds its recompiled game code (and,
with --game, the game's files), so it must never be distributed: never add one
to CI artifacts, releases or anything uploaded (CLAUDE.md, "Legal"). The runtime
and this script are what the project ships; the game is yours.

This script never runs the title. It only reads the executable (otool) and
writes the bundle (copies, install_name_tool, codesign); run() refuses any
other program, and every input is checked before anything is written. Run a
title only through the test harness (on this project's agents' Macs,
run-title.sh), never from a build step: see docs/GETTING_STARTED.md.
--dry-run prints every step and changes nothing.

What goes in <name>.app/Contents:

    MacOS/<exe>                    the title, with an rpath to ../Frameworks
    Frameworks/libvulkan.1.dylib   the Vulkan loader   } from $VULKAN_SDK/lib or
    Frameworks/libMoltenVK.dylib   the Vulkan driver   } /usr/local/lib (the
    Frameworks/libdxcompiler.dylib HLSL -> SPIR-V      } SDK's system install)
    Frameworks/<other dylibs>      every non-system library the executable
                                   links (Homebrew's libcrypto today), re-pointed
                                   at @rpath with install_name_tool
    Resources/vulkan/icd.d/MoltenVK_icd.json   the driver manifest, pointing at
                                   ../../../Frameworks/libMoltenVK.dylib
    Resources/game/                only with --game (an APFS clone, so no space)
    Info.plist                     NSHighResolutionCapable, games category

How the pieces are found at run time: src/d3d/rhi_vulkan.c dlopens the loader
from ../Frameworks beside the executable and points it at the bundle's
manifest alone (VK_DRIVER_FILES, unless you set drivers yourself), so a Vulkan
SDK installed on the same Mac is not loaded beside it; rhi_vulkan_dxc.cpp finds
the compiler in ../Frameworks too. The game folder is looked for beside the
.app first, then in Resources/game (templates/new-game/src/main.c, find_game);
prefer beside the .app, because a title writes its saves into its game folder
(UDATA) and anything written inside the bundle breaks its signature. A bundled
run logs to ~/Library/Logs/xboxrecomp/<exe>.log, never into the bundle.

FFmpeg (XMV movies) is not bundled: Homebrew builds it with --enable-gpl. The
app finds a Homebrew FFmpeg on its own (src/video/xmv_decode.c) and otherwise
says once that movies are skipped.

The bundle is signed ad hoc (codesign --sign -) and verified with --strict:
enough to run on this Mac, not to hand to anyone else.
"""
import argparse
import json
import os
import pathlib
import plistlib
import shutil
import subprocess
import sys

SYSTEM_PREFIXES = ("/usr/lib/", "/System/")
# The only programs this script starts, by absolute path so nothing on PATH
# can stand in for one. Anything else -- the title above all -- is refused,
# so no mistake in how it is called can run a game.
ALLOWED_TOOLS = {
    "otool": "/usr/bin/otool",
    "install_name_tool": "/usr/bin/install_name_tool",
    "codesign": "/usr/bin/codesign",
    "cp": "/bin/cp",
}
DRY = False


def die(msg):
    sys.exit(f"make_macos_app: {msg}")


def run(*cmd, capture=False, writes=True):
    """Run one of ALLOWED_TOOLS. `writes`: it changes something, so under
    --dry-run it is printed, not run (reads always run)."""
    if not cmd or any(not isinstance(c, str) or c == "" for c in cmd):
        die(f"refusing a command with an empty part: {cmd!r}")
    if cmd[0] not in ALLOWED_TOOLS:
        die(f"refusing to run {cmd[0]!r}: this script only runs {sorted(ALLOWED_TOOLS)}")
    cmd = (ALLOWED_TOOLS[cmd[0]],) + tuple(cmd[1:])
    if DRY and writes:
        print("  would run: " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
        return ""
    r = subprocess.run(cmd, check=True, text=True,
                       stdout=subprocess.PIPE if capture else None)
    return r.stdout if capture else None


def step(desc, fn=None):
    if DRY:
        print("  would " + desc)
    elif fn:
        fn()


def rpaths(path):
    out = run("otool", "-l", str(path), capture=True, writes=False).splitlines()
    found = []
    for i, line in enumerate(out):
        if line.strip() == "cmd LC_RPATH" and i + 2 < len(out):
            found.append(out[i + 2].strip().split(" (offset")[0].replace("path ", "", 1))
    return found


def add_rpath(path, rp, have=None):
    if rp not in (have if have is not None else rpaths(path)):
        run("install_name_tool", "-add_rpath", rp, str(path))


def linked_libraries(path):
    """The install names a Mach-O file links, its own id left out."""
    out = run("otool", "-L", str(path), capture=True, writes=False).splitlines()[1:]
    own = None
    try:
        own = run("otool", "-D", str(path), capture=True, writes=False).splitlines()[-1].strip()
    except (subprocess.CalledProcessError, IndexError):
        pass
    libs = []
    for line in out:
        name = line.strip().split(" (compatibility")[0]
        if name and name != own and name not in libs:
            libs.append(name)
    return libs


def find_sdk_file(rel):
    dirs = []
    if os.environ.get("VULKAN_SDK"):
        dirs.append(pathlib.Path(os.environ["VULKAN_SDK"]))
    dirs.append(pathlib.Path("/usr/local"))
    for d in dirs:
        p = d / rel
        if p.exists():
            return p.resolve()
    return None


def copy_file(src, dst):
    def do():
        shutil.copy2(src, dst)
        os.chmod(dst, 0o755)
    step(f"copy {src} -> {dst}", do)


def bundle_dependencies(source, binary, frameworks, done):
    """Copy every non-system library `source` links into Frameworks and point
    `binary` (its copy in the bundle) and them, recursively, at @rpath/<name>.
    Reads from `source`, so a dry run sees the same tree a real one does."""
    for lib in linked_libraries(source):
        if lib.startswith(SYSTEM_PREFIXES) or lib.startswith("@"):
            continue
        name = pathlib.Path(lib).name
        if name not in done:
            real = pathlib.Path(lib)
            if not real.exists():
                die(f"{source.name} links {lib}, which does not exist")
            dst = frameworks / name
            copy_file(real.resolve(), dst)
            run("install_name_tool", "-id", f"@rpath/{name}", str(dst))
            add_rpath(dst, "@loader_path", have=rpaths(real.resolve()))
            done[name] = dst
            bundle_dependencies(real.resolve(), dst, frameworks, done)
        run("install_name_tool", "-change", lib, f"@rpath/{name}", str(binary))


def main():
    global DRY
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("exe", help="the title's built executable")
    ap.add_argument("--name", help="the app's name (default: the executable's)")
    ap.add_argument("--out", help="where <name>.app goes (default: beside the exe)")
    ap.add_argument("--bundle-id", help="CFBundleIdentifier (default: org.xboxrecomp.<exe>)")
    ap.add_argument("--version", default="0.9.0")
    ap.add_argument("--game", help="a game folder (with default.xbe) to clone into Contents/Resources/game")
    ap.add_argument("--no-sign", action="store_true", help="skip the ad-hoc signature")
    ap.add_argument("--dry-run", action="store_true", help="print every step, change nothing")
    a = ap.parse_args()
    DRY = a.dry_run

    # Every input checked before anything is written: an empty or missing
    # one is an error, never a default that happens to work.
    if sys.platform != "darwin":
        die("macOS only")
    for flag, value in (("exe", a.exe), ("--name", a.name), ("--out", a.out),
                        ("--bundle-id", a.bundle_id), ("--game", a.game), ("--version", a.version)):
        if value is not None and value.strip() == "":
            die(f"{flag} is empty")
    exe = pathlib.Path(a.exe).resolve()
    if not exe.is_file() or not os.access(exe, os.X_OK):
        die(f"no executable at {exe}")
    if exe.suffix in (".py", ".sh"):
        die(f"{exe} is a script, not a title")
    name = a.name if a.name is not None else exe.name
    if "/" in name:
        die(f"--name {name!r} contains a slash")
    out = pathlib.Path(a.out).resolve() if a.out is not None else exe.parent
    if not out.is_dir():
        die(f"--out {out} is not a directory")
    game = None
    if a.game is not None:
        game = pathlib.Path(a.game).resolve()
        if not (game / "default.xbe").is_file():
            die(f"no default.xbe in {game}")
    pieces = {
        "libvulkan.1.dylib": find_sdk_file("lib/libvulkan.1.dylib"),
        "libMoltenVK.dylib": find_sdk_file("lib/libMoltenVK.dylib"),
        "libdxcompiler.dylib": find_sdk_file("lib/libdxcompiler.dylib"),
    }
    missing = [k for k, v in pieces.items() if not v]
    if missing:
        die("not found in $VULKAN_SDK/lib or /usr/local/lib: " + ", ".join(missing)
            + " (install the Vulkan SDK, or set VULKAN_SDK)")

    app = out / f"{name}.app"
    contents = app / "Contents"
    macos, frameworks, resources = contents / "MacOS", contents / "Frameworks", contents / "Resources"
    if DRY:
        print(f"dry run: {app}")

    if app.exists():
        step(f"remove the old {app}", lambda: shutil.rmtree(app))
    for d in (macos, frameworks, resources / "vulkan" / "icd.d"):
        step(f"make {d}", lambda d=d: d.mkdir(parents=True))

    binary = macos / exe.name
    copy_file(exe, binary)

    # The renderer's run-time pieces, which nothing links (they are dlopened).
    for dst_name, src in pieces.items():
        copy_file(src, frameworks / dst_name)

    icd_src = find_sdk_file("share/vulkan/icd.d/MoltenVK_icd.json")
    icd = json.loads(icd_src.read_text()) if icd_src else {
        "file_format_version": "1.0.0", "ICD": {"api_version": "1.4.0", "is_portability_driver": True}}
    icd["ICD"]["library_path"] = "../../../Frameworks/libMoltenVK.dylib"
    icd_dst = resources / "vulkan" / "icd.d" / "MoltenVK_icd.json"
    step(f"write {icd_dst} (library_path ../../../Frameworks/libMoltenVK.dylib)",
         lambda: icd_dst.write_text(json.dumps(icd, indent=4) + "\n"))

    # Everything the executable itself links from outside the system.
    done = {}
    bundle_dependencies(exe, binary, frameworks, done)
    add_rpath(binary, "@executable_path/../Frameworks", have=rpaths(exe))

    if game:
        # An APFS clone: instant, and no space until either copy is written.
        run("cp", "-cR", str(game), str(resources / "game"))

    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleExecutable": exe.name,
        "CFBundleIdentifier": a.bundle_id or f"org.xboxrecomp.{exe.name}",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": name[:15],
        "CFBundleDisplayName": name,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": a.version,
        "CFBundleVersion": a.version,
        "LSMinimumSystemVersion": "13.0",
        "LSApplicationCategoryType": "public.app-category.games",
        "NSHighResolutionCapable": True,
        "NSSupportsAutomaticGraphicsSwitching": True,
    }

    def write_plist():
        with open(contents / "Info.plist", "wb") as f:
            plistlib.dump(info, f)
    step(f"write {contents / 'Info.plist'}", write_plist)

    if not a.no_sign:
        # install_name_tool invalidated the copies' signatures; one ad-hoc
        # signature over the whole bundle, inside out.
        run("codesign", "--force", "--deep", "--sign", "-", str(app))
        run("codesign", "--verify", "--strict", "--verbose=2", str(app))

    print(f"{'(dry run) ' if DRY else ''}{app}")
    print(f"  bundled: {', '.join(sorted(list(pieces) + list(done)))}")
    print("  for this machine only -- it contains the title's recompiled code; never distribute it")
    print(f"  game folder: {app.parent / 'game'} (beside the app), else {resources / 'game'}")


if __name__ == "__main__":
    main()
