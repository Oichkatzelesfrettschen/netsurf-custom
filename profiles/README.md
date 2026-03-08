# Build Profiles

Tracked build and runtime profiles for repo-root low-spec work.

- `hostdev`: existing developer-oriented native flow.
- `core4m`: no-JS low-memory profile for framebuffer builds and monkey
  smoke verification, with the direct `linux` framebuffer surface as
  the intended shipping frontend.
- `script16m`: optional Duktape-backed profile with a hard
  `js_heap_limit` and reduced feature set.

Build-time flags live in `*.mk`.
Runtime defaults live in `Choices.*` and are injected through the
`NETSURF_CHOICES` environment variable by repo-root profile targets.

For profiles that keep conservative shipping defaults but need a more
capable acceptance harness, repo-root suite targets will prefer an
optional `Choices.<profile>.suite` overlay.
