# syntax=docker/dockerfile:1
# check=skip=FromPlatformFlagConstDisallowed
#
# Build environment for the Animal Forest PSP port: every tool build.sh needs, at
# the versions the port was developed with, so nothing has to be installed by hand.
# It holds no game data and not even this repository: ./docker-build.sh mounts the
# repository and your dumps into it. See BUILDING.md ("Building with Docker").
#
# It is linux/amd64 on purpose: the decomp downloads x86-64 Linux builds of the IDO
# compilers. On an Apple Silicon Mac or an ARM machine Docker runs it emulated, which
# is slower but works.
FROM --platform=linux/amd64 ubuntu:24.04

# PSPDEV: the PSP toolchain (psp-gcc 15.2, PSPSDK, pack-pbp, ...), prebuilt for Ubuntu.
# v20260701 is the release the port was developed and tested on hardware with; later
# PSPSDKs build every import as a weak one, which changes the EBOOT.
ARG PSPDEV_VERSION=v20260701
ARG PSPDEV_SHA256=f8f2f2235995836188e5fce2e6225c4b17a47232ea82dd850dbf7a5d99c90587
# psp-media-engine-custom-core: the Media Engine library the audio runs on.
ARG MECORE_COMMIT=6c9c4a351559ec8137c22917c4a56ed1f3f00fcf

ENV DEBIAN_FRONTEND=noninteractive

# build-essential, clang, binutils-mips-linux-gnu, python3-venv: the zeldaret/af decomp;
# cmake, ninja-build, a C++20 compiler (g++ 13): N64Recomp; perl: scripts/recompile.sh;
# xxd: me-core's build; the lib* packages are what the PSPDEV binaries link against.
RUN apt-get update && apt-get install -y --no-install-recommends \
        binutils-mips-linux-gnu build-essential ca-certificates clang cmake curl git \
        ninja-build perl python3 python3-pip python3-venv wget xxd \
        libarchive13t64 libcurl4t64 libgmp10 libisl23 libmpc3 libmpfr6 libncursesw6 \
        libreadline8t64 libusb-0.1-4 libzstd1 zlib1g \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL -o /tmp/pspdev.tar.gz \
        "https://github.com/pspdev/pspdev/releases/download/${PSPDEV_VERSION}/pspdev-ubuntu-latest-x86_64.tar.gz" \
    && echo "${PSPDEV_SHA256}  /tmp/pspdev.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/pspdev.tar.gz -C /usr/local \
    && rm /tmp/pspdev.tar.gz
ENV PSPDEV=/usr/local/pspdev
ENV PATH="${PSPDEV}/bin:${PATH}"

# Installs libme-core.a and its headers into $PSPDEV/psp, and keeps kcall.prx (the
# kernel module the Media Engine audio needs on a real PSP): build.sh copies it into
# dist/AFPSP because KCALL_PRX points at it.
RUN git clone -q https://github.com/mcidclan/psp-media-engine-custom-core.git /tmp/me-core \
    && git -C /tmp/me-core checkout -q "${MECORE_COMMIT}" \
    && cmake -S /tmp/me-core -B /tmp/me-core/build -G Ninja > /dev/null \
    && cmake --build /tmp/me-core/build \
    && cmake --install /tmp/me-core/build > /dev/null \
    && install -D -m 644 /tmp/me-core/build/kernel/kcall.prx /opt/afpsp/kcall.prx \
    && rm -rf /tmp/me-core
ENV KCALL_PRX=/opt/afpsp/kcall.prx

# The repository is mounted at /src, with work/ and build/ as volumes (see
# docker-build.sh); the container runs as the host user, so these must be writable
# by anyone (a new volume takes its permissions from here).
RUN git config --system --add safe.directory '*' \
    && mkdir -p /src/work /src/build /in \
    && chmod 777 /src /src/work /src/build \
    && chmod 1777 /tmp
ENV HOME=/tmp
WORKDIR /src
ENTRYPOINT ["./build.sh"]
