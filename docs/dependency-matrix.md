# NetSurf Dependency Matrix

Feature flags controlling optional library usage. Set in
`Makefile.config` with `override NETSURF_USE_X := YES|NO|AUTO`.

## Core libraries (always required)

| Library       | pkg-config name | Arch         | Ubuntu                   |
|---------------|-----------------|--------------|--------------------------|
| libcss        | libcss          | (workspace)  | (workspace)              |
| libdom        | libdom          | (workspace)  | (workspace)              |
| libnsutils    | libnsutils      | (workspace)  | (workspace)              |
| libwapcaplet  | libwapcaplet    | (workspace)  | (workspace)              |
| libparserutils| libparserutils  | (workspace)  | (workspace)              |
| libhubbub     | libhubbub       | (workspace)  | (workspace)              |
| zlib          | zlib            | zlib         | zlib1g-dev               |
| expat         | expat           | expat        | libexpat1-dev            |

## Optional features

| Flag                   | pkg-config     | Arch            | Ubuntu               | Default | Notes              |
|------------------------|----------------|-----------------|----------------------|---------|--------------------|
| NETSURF_USE_CURL       | libcurl        | curl            | libcurl4-openssl-dev | YES     | HTTP fetcher       |
| NETSURF_USE_OPENSSL    | openssl        | openssl         | libssl-dev           | AUTO    | TLS certificates   |
| NETSURF_USE_JPEG       | (none)         | libjpeg-turbo   | libjpeg-dev          | YES     | JPEG images        |
| NETSURF_USE_JPEGXL     | libjxl         | libjxl (AUR)    | libjxl-dev           | AUTO    | JPEG XL images     |
| NETSURF_USE_PNG        | libpng         | libpng          | libpng-dev           | AUTO    | PNG images         |
| NETSURF_USE_BMP        | libnsbmp       | (workspace)     | (workspace)          | AUTO    | BMP/ICO images     |
| NETSURF_USE_GIF        | libnsgif       | (workspace)     | (workspace)          | AUTO    | GIF animations     |
| NETSURF_USE_WEBP       | libwebp        | libwebp         | libwebp-dev          | AUTO    | WebP images        |
| NETSURF_USE_NSSVG      | libsvgtiny     | (workspace)     | (workspace)          | AUTO    | SVG via libsvgtiny |
| NETSURF_USE_RSVG       | librsvg-2.0    | librsvg         | librsvg2-dev         | AUTO    | SVG via librsvg    |
| NETSURF_USE_ROSPRITE   | librosprite    | (workspace)     | (workspace)          | AUTO    | RISC OS sprites    |
| NETSURF_USE_NSPSL      | libnspsl       | (workspace)     | (workspace)          | AUTO    | Public suffix list |
| NETSURF_USE_NSLOG      | libnslog       | (workspace)     | (workspace)          | AUTO    | Filtered logging   |
| NETSURF_USE_UTF8PROC   | libutf8proc    | libutf8proc     | libutf8proc-dev      | YES     | IDN processing     |
| NETSURF_USE_DUKTAPE    | (none)         | (vendored)      | (vendored)           | YES     | JavaScript engine  |
| NETSURF_USE_HARU_PDF   | (none)         | libharu (AUR)   | libhpdf-dev          | NO      | PDF export         |
| NETSURF_USE_VIDEO      | gstreamer-0.10 | gstreamer       | libgstreamer1.0-dev  | NO      | Video playback     |
| NETSURF_USE_LIBICONV_PLUG | (none)      | (glibc built-in)| (glibc built-in)     | YES     | iconv source       |

## Frontend-specific dependencies

### GTK3 (`TARGET=gtk`)

| Library    | pkg-config    | Arch    | Ubuntu         |
|------------|---------------|---------|----------------|
| GTK+ 3     | gtk+-3.0      | gtk3    | libgtk-3-dev   |
| GThread2   | gthread-2.0   | glib2   | libglib2.0-dev |
| GModule2   | gmodule-2.0   | glib2   | libglib2.0-dev |
| librsvg    | librsvg-2.0   | librsvg | librsvg2-dev   |

### Qt6 (`TARGET=qt`)

| Library  | pkg-config | Arch     | Ubuntu           |
|----------|------------|----------|------------------|
| Qt6 Base | Qt6Core    | qt6-base | qt6-base-dev     |

### Framebuffer (`TARGET=framebuffer`)

| Library   | pkg-config   | Arch          | Ubuntu                 |
|-----------|-------------|---------------|------------------------|
| FreeType2 | freetype2   | freetype2     | libfreetype-dev        |
| SDL 1.2   | sdl         | sdl12-compat  | libsdl1.2-compat-dev   |
| libnsfb   | libnsfb     | (workspace)   | (workspace)            |

## Build tools

| Tool             | Arch              | Ubuntu             |
|------------------|-------------------|--------------------|
| C compiler       | base-devel (gcc)  | build-essential    |
| pkg-config       | pkgconf           | pkg-config         |
| git              | git               | git                |
| gperf            | gperf             | gperf              |
| flex             | flex              | flex               |
| bison            | bison             | bison              |
| perl HTML parser | perl-html-parser  | libhtml-parser-perl|
| ccache           | ccache            | ccache             |
| check (tests)    | check             | check              |
| nsgenbind        | (workspace)       | (workspace)        |

## Notes

- **(workspace)** means the library must be built from source using
  `ns-clone` and `ns-make-libs install` via env.sh. These are
  NetSurf-specific libraries not packaged by distributions.

- **(vendored)** means the source is included directly in the NetSurf
  tree (e.g., duktape.c).

- **(AUR)** means the package is available from the Arch User
  Repository, not the official repos. JPEGXL (`libjxl`) is now in
  the official Arch repos as of 2025.
