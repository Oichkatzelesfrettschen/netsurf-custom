# Building NetSurf on Arch Linux / CachyOS

## Prerequisites

Arch-based system with `pacman` available. Tested on CachyOS with
kernel 6.19.x and GCC 15.x.

## 1. Install system packages

```sh
sudo pacman -S --needed base-devel pkgconf git gperf curl expat \
  libpng libjpeg-turbo libutf8proc openssl flex bison \
  perl-html-parser ccache check gtk3 librsvg qt6-base \
  freetype2 sdl12-compat
```

Or use the env.sh helper (after step 2):

```sh
export TARGET_TOOLKIT=gtk3   # or qt6, framebuffer
ns-package-install
```

## 2. Build NetSurf libraries

Several libraries (libnspsl, libnslog, libdom, etc.) are not in the
Arch repos. Use the env.sh workspace to build them:

```sh
# IMPORTANT: unset HOST if your shell sets it to the hostname
unset HOST

export TARGET_WORKSPACE="${HOME}/dev-netsurf/workspace"
export TARGET_TOOLKIT=gtk3
source docs/env.sh

ns-clone -d           # clone dependency repos
ns-make-tools install # build nsgenbind
ns-make-libs install  # build all libraries
```

This installs headers and static libraries under
`$TARGET_WORKSPACE/inst-$(cc -dumpmachine)/`.

## 3. Build NetSurf

Source env.sh in the same shell session (it sets `PKG_CONFIG_PATH` and
`LD_LIBRARY_PATH`):

```sh
unset HOST
export TARGET_WORKSPACE="${HOME}/dev-netsurf/workspace"
export TARGET_TOOLKIT=gtk3
source docs/env.sh

# GTK3 frontend
make -j$(nproc) TARGET=gtk

# Or: Qt6 frontend
# make -j$(nproc) TARGET=qt

# Or: Framebuffer frontend
# make -j$(nproc) TARGET=framebuffer
```

The binary is produced in the current directory: `nsgtk3`, `nsqt`, or
`nsfb`.

## 4. Run tests

```sh
# Same shell session with env.sh sourced
make test
```

## 5. Makefile.config

The repository includes a `Makefile.config` that enables `-Werror` and
sets all optional features to `YES` (avoiding the AUTO evaluation-order
issue in the build system). Review and adjust as needed.

## Known issues

### HOST environment variable

If your shell sets `HOST` to the machine hostname (common in zsh),
env.sh misinterprets it as a cross-compilation target. Always
`unset HOST` before sourcing env.sh.

### AUTO feature detection vs source file inclusion

The upstream build system has a make evaluation-order issue: when a
feature is set to `AUTO`, the `pkg_config_find_and_add_enabled` macro
resolves it to `YES` at eval time, but source file lists
(`S_IMAGE_$(NETSURF_USE_X)`) are indexed at include time while the
variable is still `AUTO`. This causes link errors for image handlers.
The workaround is to set features explicitly to `YES` in
Makefile.config.

### ns-clone shallow flag

The `-s` (shallow) flag in `ns-clone` passes `--depth 1` as a single
string argument to git, which newer git versions reject. Use
`ns-clone -d` (without `-s`) as a workaround.

### Words file for tests

Some tests use `/usr/share/dict/words`. Install the `words` package
if tests fail looking for this file:

```sh
sudo pacman -S --needed words
```
