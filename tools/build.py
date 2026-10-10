#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the native FM-1 emulator from the pinned QEMU release."""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import integrate

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CACHE = ROOT / ".cache"
QEMU = integrate.QEMU
QEMU_COMMIT = "4fc49f46dc95d4a27de2509e7fceb2931e91faeb"
QEMU_SHA = "731b5681e4bb18be313231579b8efd0296c5b015fa36dc533874b639ba838016"
GLIB_URL = "https://download.gnome.org/sources/glib/2.84/glib-2.84.0.tar.xz"
GLIB_SHA = "f8823600cb85425e2815cfad82ea20fdaa538482ab74e7293d58b3f64a5aff6a"
# Static libraries the native executable links: GLib's and SDL2's.
DEPS = CACHE / "native-deps"
PACKAGES = ["meson==1.5.0", "ninja==1.11.1.4", "distlib==0.3.9", "pycotap==1.3.1"]


def fetch(url, filename, checksum):
    target = CACHE / filename
    if not target.exists():
        print(f"Downloading {filename}", flush=True)
        partial = target.with_suffix(target.suffix + ".part")
        urllib.request.urlretrieve(url, partial)
        partial.replace(target)
    actual = hashlib.sha256(target.read_bytes()).hexdigest()
    if actual != checksum:
        raise SystemExit(f"checksum mismatch: {filename}: {actual}")
    return target


def run(command, cwd, env, log):
    print(f"{log}", flush=True)
    with (CACHE / log).open("w") as out:
        result = subprocess.run(list(map(str, command)), cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
    if result.returncode:
        lines = (CACHE / log).read_text(errors="replace").splitlines()
        print("\n".join(lines[-35:]), file=sys.stderr)
        raise SystemExit(f"build step failed; inspect {CACHE / log}")


def mise_prefix(env, package):
    """The conda-forge environment mise put on PATH for package."""
    for entry in env["PATH"].split(os.pathsep):
        prefix = Path(entry).parent
        if prefix.parent.name == f"conda-{package}" and (prefix / "conda-meta").is_dir():
            return prefix
    raise SystemExit(f"conda:{package} is not on PATH: run this as `mise run build`")


def build_tools(tools, env):
    """Archive with the LLVM matching mise's clang: the system ar cannot index
    its LTO bitcode, and Meson only accepts an archiver named ar. Ask
    pkg-config for static linking flags, as every library linked is static.
    Run clang without conda's default config, whose include and library
    paths put conda's libiconv before the C library's; on Linux name the
    glibc sysroot that config supplied."""
    (tools / "bin").mkdir(parents=True, exist_ok=True)
    for name in ("ar", "ranlib"):
        target = shutil.which(f"llvm-{name}", path=env["PATH"])
        if not target:
            raise SystemExit(f"no llvm-{name} on PATH: run this as `mise run build`")
        target = Path(target)
        link = tools / "bin" / name
        if not link.is_symlink() or link.readlink() != target:
            link.unlink(missing_ok=True)
            link.symlink_to(target)
    path = os.pathsep.join(entry for entry in env["PATH"].split(os.pathsep)
                           if Path(entry) != tools / "bin")
    pkg_config = shutil.which("pkg-config", path=path)
    clang = shutil.which("clang", path=path)
    if not pkg_config or not clang:
        raise SystemExit("no pkg-config or clang on PATH: run this as `mise run build`")
    flags = "--no-default-config"
    if sys.platform != "darwin":
        def ask(option):
            return subprocess.check_output([clang, option], text=True).strip()
        install = Path(ask("-print-resource-dir")).parents[2]
        flags += f' --sysroot="{install / ask("-print-target-triple") / "sysroot"}"'
    wrapper = tools / "bin/pkg-config"
    if wrapper.is_symlink():
        wrapper.unlink()  # to pkgconf, which older builds compiled here
    for name, command in (("pkg-config", f'"{pkg_config}" --static'), ("clang", f'"{clang}" {flags}')):
        wrapper = tools / "bin" / name
        integrate.write_changed(wrapper, f'#!/bin/sh\nexec {command} "$@"\n')
        wrapper.chmod(0o755)


def static_dependencies(env, venv):
    """Static GLib, with PCRE2, zlib and (macOS) libintl from its pinned
    fallbacks, and conda-forge's static SDL2, in one prefix without shared
    libraries, so the executable carries them all. GObject, which the
    emulator does not link, takes conda-forge's libffi, as GLib's own
    fallback for it needs git."""
    lib = DEPS / "lib"
    if not (lib / "libglib-2.0.a").exists():
        # The clang wrapper, not the system's cc, against the C library the
        # emulator links.
        glib_env = dict(env, CC="clang", OBJC="clang", PKG_CONFIG_PATH=os.pathsep.join(
            [env["PKG_CONFIG_PATH"], str(mise_prefix(env, "libffi") / "lib/pkgconfig")]))
        archive = fetch(GLIB_URL, Path(GLIB_URL).name, GLIB_SHA)
        source = CACHE / Path(GLIB_URL).name.removesuffix(".tar.xz")
        if not source.exists():
            with tarfile.open(archive) as tar:
                tar.extractall(CACHE, filter="data")
        shutil.rmtree(source / "_native", ignore_errors=True)
        run([venv / "bin/meson", "setup", "_native", f"--prefix={DEPS}", "--libdir=lib",
             "--default-library=static", "--buildtype=release",
             "--force-fallback-for=pcre2,zlib", "-Dselinux=disabled", "-Dxattr=false",
             "-Dlibmount=disabled", "-Dsysprof=disabled", "-Dnls=disabled", "-Dtests=false",
             "-Dintrospection=disabled", "-Dman-pages=disabled", "-Ddocumentation=false",
             "-Dglib_debug=disabled"], source, glib_env, "glib-configure.log")
        run([venv / "bin/meson", "install", "-C", "_native"], source, glib_env, "glib-build.log")
    sdl = mise_prefix(env, "sdl2")
    shutil.copy2(sdl / "lib/libSDL2.a", lib / "libSDL2.a")
    # SDL_syswm.h includes Xlib.h on Linux, whose protocol headers conda
    # ships separately in xorgproto.
    headers = [sdl / "include/SDL2"]
    if sys.platform != "darwin":
        headers += [sdl / "include/X11", mise_prefix(env, "xorg-xorgproto") / "include/X11"]
    for directory in headers:
        shutil.copytree(directory, DEPS / "include" / directory.name, dirs_exist_ok=True)
    # Without the rpath into this cache that conda's Linux package adds.
    package = (sdl / "lib/pkgconfig/sdl2.pc").read_text().replace(str(sdl), str(DEPS))
    package = package.replace("-Wl,-rpath,${libdir}", "").replace("-Wl,--enable-new-dtags", "")
    integrate.write_changed(lib / "pkgconfig/sdl2.pc", package)


def check_dependencies(executable):
    """Reject any shared library the operating system does not provide."""
    if sys.platform == "darwin":
        listing = subprocess.check_output(["otool", "-L", str(executable)], text=True)
        names = [line.strip().split(" (", 1)[0] for line in listing.splitlines()[1:]]
        foreign = [name for name in names if not name.startswith(("/usr/lib/", "/System/Library/"))]
    else:
        listing = subprocess.check_output(["ldd", str(executable)], text=True)
        names = [line.strip().split(" ", 1)[0] for line in listing.splitlines()]
        system = ("linux-vdso.so", "/lib", "ld-linux", "libc.so", "libm.so", "libdl.so",
                  "libpthread.so", "librt.so", "libutil.so")
        foreign = [name for name in names if not name.startswith(system)]
    if foreign:
        raise SystemExit(f"the executable still needs installed libraries: {', '.join(foreign)}")


def publish_executable(build, source):
    """Publish one native executable that needs no installed libraries."""
    release = CACHE / "release"
    release.mkdir(exist_ok=True)
    shutil.rmtree(release / "lib", ignore_errors=True)
    target = release / "emulator.new"
    shutil.copy2(build / "qemu-system-pi32v2", target)
    try:
        check_dependencies(target)
    except SystemExit:
        target.unlink()
        raise
    if sys.platform == "darwin":
        subprocess.run(["codesign", "--force", "--sign", "-", str(target)], check=True)
        subprocess.run(["codesign", "--verify", "--strict", str(target)], check=True)
    target.replace(release / "emulator")
    # Publish atomically, preserving a running executable and the last good build.
    with tempfile.TemporaryDirectory(prefix=".emulator-", dir=HERE.parent) as staged:
        executable = Path(staged) / "emulator"
        shutil.copy2(release / "emulator", executable)
        executable.replace(HERE.parent / "emulator")
    shutil.copy2(ROOT / "LICENSE", release / "COPYING.GPL-2.0")
    shutil.copy2(ROOT / "LICENSES.md", release / "LICENSES.md")
    # Retain the notices of the linked libraries; QEMU ships the LGPL text.
    glib = CACHE / Path(GLIB_URL).name.removesuffix(".tar.xz")
    notices = {"GLib-LGPL-2.1.txt": source / "COPYING.LIB"}
    notices |= {"PCRE2-LICENCE.txt": path for path in glib.glob("subprojects/pcre2-*/LICENCE*")}
    for name, notice in notices.items():
        shutil.copy2(notice, release / name)
    integrate.write_changed(release / "RUNNING.txt",
        "Run: ./emulator path/to/firmware.bin\n"
        "Close the window to quit. Use --help for controls.\n"
        "The USB console writes to stdout and accepts commands from stdin.\n"
        "Python and third-party library installations are not required to run.\n"
        "Raw .bin and uncompressed .fwsc/.ufw loading is supported; firmware compatibility varies.\n"
        "For redistribution retain licensing and provide the corresponding source/build inputs.\n")
    print(f"Standalone executable: {HERE.parent / 'emulator'}")
    system = "macos" if sys.platform == "darwin" else sys.platform
    arch = {"x86_64": "amd64", "aarch64": "arm64"}.get(platform.machine(), platform.machine())
    package = CACHE / f"fm1-emulator-{system}-{arch}.tar.gz"
    with tarfile.open(package, "w:gz") as archive:
        for item in sorted(release.iterdir()):
            archive.add(item, arcname=item.name)
    print(f"Distribution archive: {package}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reconfigure", action="store_true")
    parser.add_argument("--standalone", action="store_true",
                        help="publish ./emulator and a distribution archive")
    args = parser.parse_args()
    build = CACHE / ("build-standalone" if args.standalone else "build")
    CACHE.mkdir(exist_ok=True)
    # An older Ninja build remains tied to its original source tree. Do not
    # rebuild it and report the new pin as though an upgrade occurred.
    project_info = build / "meson-info/intro-projectinfo.json"
    if project_info.exists() and json.loads(project_info.read_text())["version"] != QEMU:
        raise SystemExit(f"cached build belongs to another QEMU release; preserve "
                         f"{build} elsewhere and rebuild QEMU {QEMU}")
    env = dict(os.environ)
    env["PIP_CACHE_DIR"] = str(CACHE / "pip-cache")
    env["PYTHONNOUSERSITE"] = "1"
    # This interpreter must have been selected with mise (see README).
    if sys.version_info[:2] != (3, 13):
        raise SystemExit("run with: mise run build")
    archive = fetch(f"https://download.qemu.org/qemu-{QEMU}.tar.xz", f"qemu-{QEMU}.tar.xz", QEMU_SHA)
    source = CACHE / f"qemu-{QEMU}"
    if not source.exists():
        with tarfile.open(archive) as tar:
            # EDK2 is unused by this target; its upstream X11 include link
            # points outside the archive and is rejected by the data filter.
            excluded = f"qemu-{QEMU}/roms/edk2/EmulatorPkg/Unix/Host/X11IncludeHack"
            members = [member for member in tar if member.name != excluded]
            tar.extractall(CACHE, members=members, filter="data")
    venv = CACHE / "python"
    if not venv.exists():
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
    if not (venv / ".requirements").exists() or (venv / ".requirements").read_text() != "\n".join(PACKAGES):
        run([venv / "bin/pip", "install", *PACKAGES], HERE, env, "python-dependencies.log")
        (venv / ".requirements").write_text("\n".join(PACKAGES))
    tools = CACHE / "tools"
    env["PATH"] = os.pathsep.join([str(venv / "bin"), str(tools / "bin"), env["PATH"]])
    env["PKG_CONFIG_PATH"] = str(DEPS / "lib/pkgconfig")
    build_tools(tools, env)
    static_dependencies(env, venv)
    integrate.main()
    build.mkdir(exist_ok=True)
    # GNU ld cannot link LLVM bitcode; macOS's linker can. Linux links
    # libgcc in too, leaving only the C library dynamic.
    ldflags = "" if sys.platform == "darwin" else "-fuse-ld=lld -static-libgcc"
    configure = [source / "configure", "--target-list=pi32v2-softmmu", f"--python={venv / 'bin/python'}",
                 "--cc=clang", "--objcc=clang", "--host-cc=clang",
                 "--without-default-features", "--enable-tcg", "--disable-fdt", "--disable-docs",
                 "--disable-user", "--disable-tools", "--disable-guest-agent", "--disable-slirp",
                 "--disable-capstone", "--enable-werror",
                 "--enable-sdl", "--audio-drv-list=sdl", "--enable-lto",
                 f"--extra-ldflags={ldflags}"]
    # Configure again whenever its command line changes.
    stamp = build / "fm1-configure.txt"
    command = "\n".join(map(str, configure))
    if args.reconfigure or not (build / "build.ninja").exists() or \
            not stamp.exists() or stamp.read_text() != command:
        run(configure, build, env, "configure.log")
        stamp.write_text(command)
    run([venv / "bin/ninja", "-j8", "qemu-system-pi32v2"], build, env, "build.log")
    print(f"Built {build / 'qemu-system-pi32v2'} (QEMU {QEMU}, {QEMU_COMMIT})")
    if args.standalone:
        publish_executable(build, source)


if __name__ == "__main__":
    main()
