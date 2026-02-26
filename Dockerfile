# NetSurf build container (Arch Linux)
#
# WHY: Provides a reproducible build environment for NetSurf on Arch Linux,
#      independent of the host system's package versions.
#
# Usage:
#   docker build -t netsurf-arch .
#   docker run --rm -v $(pwd):/src netsurf-arch make -C /src TARGET=gtk

FROM archlinux:latest

# Install system packages
RUN pacman -Syu --noconfirm --needed \
    base-devel \
    pkgconf \
    git \
    gperf \
    curl \
    expat \
    libpng \
    libjpeg-turbo \
    libutf8proc \
    openssl \
    flex \
    bison \
    perl-html-parser \
    ccache \
    check \
    gtk3 \
    librsvg \
    qt6-base \
    freetype2 \
    sdl12-compat

# Build workspace for NetSurf-specific libraries
WORKDIR /build
COPY docs/env.sh /build/docs/env.sh

ENV TARGET_WORKSPACE=/build/workspace
ENV TARGET_TOOLKIT=gtk3
ENV REPO_BASE_URI=https://github.com/netsurf-browser

RUN bash -c '\
    unset HOST && \
    source /build/docs/env.sh && \
    ns-clone -d && \
    ns-make-tools install && \
    ns-make-libs install \
'

# Set runtime environment
ENV PKG_CONFIG_PATH=/build/workspace/inst-x86_64-pc-linux-gnu/lib/pkgconfig
ENV LD_LIBRARY_PATH=/build/workspace/inst-x86_64-pc-linux-gnu/lib

WORKDIR /src
