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
```

PKG_CONFIG_PATH must include the workspace -- env.sh sets this. Always
source env.sh before running make.

## Standards

- Warnings treated as errors: `-Werror` is set in `Makefile.config`
- All optional features set to `YES` explicitly (avoids AUTO eval-order bug)
- CI should remain green on Ubuntu; Arch CI target added in build.yaml

## Common issues

1. **HOST set to hostname**: `unset HOST` before sourcing env.sh
2. **AUTO features missing from link**: Set to `YES` in Makefile.config
3. **ns-clone -s fails**: Use `ns-clone -d` (drop shallow flag)
4. **Tests need PKG_CONFIG_PATH**: Run make in the same shell that sourced env.sh

## References

- `docs/building-Arch.md` -- Arch-specific build guide
- `docs/dependency-matrix.md` -- Feature flag to package mapping
- `docs/env.sh` -- Workspace setup (now includes pacman support)
- `Makefile.config` -- Local build configuration
