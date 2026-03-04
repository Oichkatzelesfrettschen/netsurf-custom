# NetSurf Custom

Personal fork of netsurf-browser/netsurf for Arch Linux / CachyOS development.

## Remotes

- `origin` -- Oichkatzelesfrettschen/netsurf-custom (fork, push here)
- `upstream` -- netsurf-browser/netsurf (read-only, merge from here)

Weekly sync: `git fetch upstream && git merge upstream/master && git push origin master`

## Build

Requires env.sh workspace for NetSurf-specific libraries. See
`docs/building-Arch.md` for full setup.

```sh
# First time: build workspace libs
unset HOST
export TARGET_WORKSPACE="${HOME}/dev-netsurf/workspace"
export TARGET_TOOLKIT=gtk3
source docs/env.sh
ns-clone -d && ns-make-tools install && ns-make-libs install

# Daily use (same shell session after sourcing env.sh)
make -j$(nproc) TARGET=gtk     # GTK3
make -j$(nproc) TARGET=qt      # Qt6
make -j$(nproc) TARGET=framebuffer
make test

# Generate compile_commands.json for clangd / clang-tidy / scan-build
# WHY: bear records every compilation command so tools find all -I paths.
# Run in the same shell session that has sourced docs/env.sh.
make compile-db
# or directly: bear -- make -j$(nproc) TARGET=gtk
```

PKG_CONFIG_PATH must include the workspace -- env.sh sets this. Always
source env.sh before running make.

## Standards

- Warnings treated as errors: `-Werror` is set in `Makefile.config`
- All optional features set to `YES` explicitly (avoids AUTO eval-order bug)
- CI should remain green on Ubuntu; Arch CI target added in build.yaml

## Build Quirks

These three issues will silently break builds if missed:

1. **`HOST` must be unset**: Many shells (especially zsh on Arch) export `HOST`
   to the machine hostname.  NetSurf's build system treats a non-empty `HOST`
   as a cross-compilation request and selects the wrong toolchain.
   Always run `unset HOST` before sourcing `docs/env.sh`.

2. **Set features to `YES`, not `AUTO`**: The `pkg_config_find_and_add_enabled`
   macro evaluates `S_IMAGE_$(NETSURF_USE_X)` at Makefile include time, before
   the variable has been resolved from `AUTO` to `YES`.  This causes the feature
   to be excluded from the link even though the library is present.
   Set every feature you want explicitly to `YES` in `Makefile.config`
   (see `Makefile.config.example` for the reference set).

3. **`env.sh` must be sourced in the same shell that runs `make`**: `env.sh`
   sets `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` to point at workspace libraries.
   Those environment variables are not inherited across separate shell invocations.
   Always use a single Bash invocation: `source docs/env.sh && make TARGET=gtk`.

4. **librosprite causes linker failure**: `nssprite_init` is undefined unless
   librosprite is installed in the workspace.  `ns-make-libs` does not build it
   by default.  Keep `NETSURF_USE_ROSPRITE := NO` in `Makefile.config`.

5. **Standard/enhanced modes share the same `nsmonkey` binary**: Each mode has
   its own build directory (`build/Linux-monkey/` vs `build/Linux-monkey-enhanced/`)
   so no `make clean` is needed when switching modes -- both are warm in ccache.
   The binary is always written to `./nsmonkey`; the last mode built is what runs.
   ccache is auto-detected by `Makefile.tools` and provides ~74% hit rate.
   Switching: `make -j$(nproc) TARGET=monkey` (standard) or
   `make -j$(nproc) TARGET=monkey NETSURF_JS_ENGINE=enhanced` (enhanced).

## Common issues

1. **HOST set to hostname**: `unset HOST` before sourcing env.sh
2. **AUTO features missing from link**: Set to `YES` in Makefile.config
3. **ns-clone -s fails**: Use `ns-clone -d` (drop shallow flag)
4. **Tests need PKG_CONFIG_PATH**: Run make in the same shell that sourced env.sh

## Upstream Conflicts

When merging `upstream/master`, expect conflicts in these files:

**Fork-local files (we own them entirely -- always keep our version):**
- `content/handlers/javascript/duktape/History.bnd` -- new file, no upstream equivalent
- `content/handlers/javascript/duktape/polyfill.js` -- new file
- `content/handlers/javascript/duktape/Element.bnd` (innerHTML getter/setter additions)
- `desktop/browser_history.c` / `desktop/browser_history.h` (pushState/replaceState additions)

**Shared files (merge carefully -- keep our additions):**
- `content/handlers/css/select.c` -- upstream owns the file; we added hover/active/focus/
  checked/disabled/enabled/target callbacks.  On conflict, keep upstream's changes AND
  our added callback bodies.
- `content/handlers/javascript/duktape/Window.bnd` -- upstream owns; we added the
  `history()` getter (lines ~374-377) and the `RING_ITERATE_STOP` typo fix (line 244).
- `content/handlers/javascript/duktape/netsurf.bnd` -- upstream owns; we added
  `#include "History.bnd"` between Location and Navigator.

**Merge strategy:**
```sh
git fetch upstream
git merge upstream/master
# Resolve conflicts in the shared files listed above using git mergetool
# After resolution: make -j$(nproc) TARGET=gtk && make test
```

## References

- `docs/building-Arch.md` -- Arch-specific build guide
- `docs/dependency-matrix.md` -- Feature flag to package mapping
- `docs/env.sh` -- Workspace setup (now includes pacman support)
- `docs/js-api-notes.md` -- JS API limitations, deviations, and implementation status
- `Makefile.config` -- Local build configuration (gitignored; copy from Makefile.config.example)
- `tools/lacunae.py` -- JS binding gap analysis (scan/gaps/diff/spec-coverage)
- `tools/fetch-spec-idl.sh` -- Download WebIDL specs from w3c/webref (monthly)
- `tools/http-audit.py` -- HTTP protocol compliance audit (RFC 9110/9111)
- `docs/spec-coverage.md` -- Auto-generated spec coverage report
- `docs/http-compliance.md` -- Auto-generated HTTP compliance report
