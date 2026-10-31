#!/usr/bin/env bash
# install_deps.sh - Install build dependencies for the native Linux build of
# the xboxrecomp runtime libraries.
#
# Detects the host family from /etc/os-release and uses its package manager:
#   Debian/Ubuntu (incl. WSL) -> apt
#   Arch/CachyOS/Manjaro      -> pacman
#   Fedora/RHEL               -> dnf
#
#   bash tools/linux/install_deps.sh
#
# Requires sudo (will prompt for a password).
set -euo pipefail

# Overridable so the self-check below can feed it fake os-release files.
OS_RELEASE="${OS_RELEASE:-/etc/os-release}"

# Same dependencies throughout, spelled per distro:
#   build toolchain + cmake + pkg-config
#   SDL3 and SDL_shadercross are fetched and built by CMake (cmake/xbox_sdl3.cmake);
#           what they need from the system is git, python3 and the window-system,
#           audio and Vulkan development headers listed below.
#   OpenSSL - crypto, replaces bcrypt
#   ffmpeg  - video decode/demux/scale
detect_family() {
    local id id_like
    [ -r "$OS_RELEASE" ] || return 1
    id=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID:-}")
    id_like=$(. "$OS_RELEASE" 2>/dev/null && echo "${ID_LIKE:-}")

    case " $id $id_like " in
        *" debian "*|*" ubuntu "*) echo debian ;;
        *" arch "*|*" archlinux "*) echo arch ;;
        *" fedora "*|*" rhel "*|*" centos "*) echo fedora ;;
        *) return 1 ;;
    esac
}

family=$(detect_family) || {
    echo "Could not identify the distro from $OS_RELEASE." >&2
    echo "Install these by hand: a C/C++ toolchain, cmake, git, python3, pkg-config," >&2
    echo "OpenSSL and ffmpeg (with headers), and the X11/Wayland, ALSA/PulseAudio," >&2
    echo "udev and Vulkan development headers SDL3 builds against." >&2
    exit 1
}

case "$family" in
    debian)
        PKGS=(cmake build-essential git python3 pkg-config libssl-dev
              libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
              libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev
              libxss-dev libxfixes-dev libwayland-dev libxkbcommon-dev
              libdrm-dev libgbm-dev libasound2-dev libpulse-dev libudev-dev
              libvulkan-dev)
        echo "Detected Debian/Ubuntu. Installing ${#PKGS[@]} packages..."
        sudo apt-get update
        sudo apt-get install -y "${PKGS[@]}"
        ;;
    arch)
        # base-devel covers gcc/make/pkgconf; ffmpeg ships headers.
        PKGS=(base-devel cmake git python openssl ffmpeg libx11 libxext
              libxrandr libxcursor libxi libxss libxfixes wayland libxkbcommon
              libdrm mesa alsa-lib libpulse systemd-libs vulkan-headers
              vulkan-icd-loader)
        echo "Detected Arch. Installing ${#PKGS[@]} packages..."
        sudo pacman -S --needed "${PKGS[@]}"
        ;;
    fedora)
        PKGS=(cmake gcc gcc-c++ make git python3 pkgconf-pkg-config openssl-devel
              ffmpeg-devel libX11-devel libXext-devel libXrandr-devel
              libXcursor-devel libXi-devel libXScrnSaver-devel libXfixes-devel
              wayland-devel libxkbcommon-devel libdrm-devel mesa-libgbm-devel
              alsa-lib-devel pulseaudio-libs-devel systemd-devel vulkan-headers
              vulkan-loader-devel)
        echo "Detected Fedora/RHEL. Installing ${#PKGS[@]} packages..."
        echo "(ffmpeg-devel needs RPM Fusion enabled.)"
        sudo dnf install -y "${PKGS[@]}"
        ;;
esac

echo
echo "Done. Build deps installed."
