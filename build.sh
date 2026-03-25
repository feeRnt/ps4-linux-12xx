#!/usr/bin/env bash

# Usage:
#   ./build.sh
#     → Interactive menu.
#   ./build.sh --option N
#     → Non-interactive: N=1 (build), 2 (fetch), 3 (both), etc.
#   ./build.sh --option N use=Server
#     → Non-interactive, force build profile to "server" or "general" (case-insensitive).
#   ./build.sh --option 7
#     → Show/switch build profile non-interactively.

set -euo pipefail

OUTPUT_DIR="${PWD}/out"
FIRMWARE_DIR="${PWD}/extra_firmware"
BASE_URL="https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main"

export KCFLAGS="-march=btver2 -mtune=btver2 -O3"
export KAFLAGS="-march=btver2 -mtune=btver2 -O3"
export HOSTCFLAGS="-Wno-error=incompatible-pointer-types-discards-qualifiers"

PROFILE="server"

JOBS=$(nproc)
MAX_JOBS=$(nproc)

# Parse optional build profile argument: use=Server or use=General
if [[ $# -ge 3 && "$3" =~ ^use= ]]; then
    PROFILE_ARG="${3#use=}"
    if [[ "${PROFILE_ARG,,}" == "server" ]]; then
        PROFILE="server"
    elif [[ "${PROFILE_ARG,,}" == "general" ]]; then
        PROFILE="general"
    else
        echo "Unknown build profile: ${PROFILE_ARG}. Valid: Server, General"
        exit 1
    fi
fi

if [[ $# -ge 2 && "$1" == "--option" ]]; then
    CHOICE="$2"
    case "$CHOICE" in
        1) DO_BUILD=1; DO_FETCH=0 ;;
        2) DO_BUILD=0; DO_FETCH=1 ;;
        3) DO_BUILD=1; DO_FETCH=1 ;;
        4|5|6)
            echo "--option $CHOICE is not supported in non-interactive mode."
            exit 1
            ;;
        7)
            echo "Current build profile: ${PROFILE}"
            echo "Switch profile? (y/n): "
            read -r SWITCH
            if [[ "$SWITCH" =~ ^[Yy]$ ]]; then
                if [[ "$PROFILE" == "server" ]]; then
                    PROFILE="general"
                else
                    PROFILE="server"
                fi
                echo "Profile switched to: ${PROFILE}"
            fi
            exit 0
            ;;
        *)
            echo "Invalid --option argument: $CHOICE"
            exit 1
            ;;
    esac
    SKIP_MENU=1
else
    SKIP_MENU=0
fi

if [[ "$SKIP_MENU" == "0" ]]; then
while true; do
    clear
    echo -e "\e[1;35m╔══════════════════════════════════════════════════╗\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;37mPS4-Linux Strawberry Builder\e[0m                     \e[1;35m║\e[0m"
    echo -e "\e[1;35m╠══════════════════════════════════════════════════╣\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;32m1)\e[0m Build bzImage                                 \e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;32m2)\e[0m Fetch firmware blobs                          \e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;32m3)\e[0m Both (fetch + build)                          \e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;32m4)\e[0m Threads to use: \e[1;33m$(printf "%-29s" "${JOBS} / ${MAX_JOBS}")\e[0m \e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;32m6)\e[0m Build profile: \e[1;33m${PROFILE}\e[0m$(printf "%-22s" "")\e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;31m5)\e[0m Quit                                          \e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;36m7)\e[0m Show/switch build profile                      \e[1;35m║\e[0m"
    echo -e "\e[1;35m╚══════════════════════════════════════════════════╝\e[0m"
    echo ""
    read -p "Select option [1-7]: " CHOICE

    case "$CHOICE" in
        1) DO_BUILD=1; DO_FETCH=0; break ;;
        2) DO_BUILD=0; DO_FETCH=1; break ;;
        3) DO_BUILD=1; DO_FETCH=1; break ;;
        4) 
            read -p "Enter number of threads (1-${MAX_JOBS}): " NEW_JOBS
            if [[ "$NEW_JOBS" =~ ^[0-9]+$ ]] && [ "$NEW_JOBS" -ge 1 ] && [ "$NEW_JOBS" -le "$MAX_JOBS" ]; then
                JOBS=$NEW_JOBS
            else
                echo -e "\e[1;31m[!] Invalid input.\e[0m Press enter to continue."
                read -r
            fi
            ;;
        5) echo "Exiting."; exit 0 ;;
        6)
            echo ""
            echo "Select build profile:"
            echo "  1) Server (max throughput, headless)"
            echo "  2) General Use (desktop latency tweaks)"
            read -p "Profile [1-2]: " PROFILE_CHOICE
            if [[ "$PROFILE_CHOICE" == "1" ]]; then
                PROFILE="server"
            elif [[ "$PROFILE_CHOICE" == "2" ]]; then
                PROFILE="general"
            else
                echo -e "\e[1;31m[!] Invalid input.\e[0m Press enter to continue."
                read -r
            fi
            ;;
        7)
            echo ""
            echo "Current build profile: ${PROFILE}"
            echo "Switch profile? (y/n): "
            read -r SWITCH
            if [[ "$SWITCH" =~ ^[Yy]$ ]]; then
                if [[ "$PROFILE" == "server" ]]; then
                    PROFILE="general"
                else
                    PROFILE="server"
                fi
                echo "Profile switched to: ${PROFILE}"
                sleep 1
            fi
            ;;
        *) echo -e "\e[1;31m[!] Invalid option.\e[0m"; sleep 1 ;;
    esac
done
fi

MAKE_OPTS=(
    -j"${JOBS}"
    LLVM=1
    ARCH=x86_64
    HOSTCFLAGS="${HOSTCFLAGS}"
)

if [[ ! -f Makefile ]] || ! grep -q "KERNELRELEASE" Makefile 2>/dev/null; then
    echo -e "\e[1;31mERROR:\e[0m Run this from the kernel source root (ps4-linux-12xx/)." >&2
    exit 1
fi

if [[ ! -f .config ]]; then
    if [[ -f config ]]; then
        echo -e "\e[1;34m[*]\e[0m Moving 'config' → '.config'"
        mv config .config
    else
        echo -e "\e[1;31mERROR:\e[0m No .config found." >&2
        exit 1
    fi
fi

if [[ "$DO_FETCH" == "1" ]]; then
    CONFIG_LINE=$(grep -E '^CONFIG_EXTRA_FIRMWARE=' .config 2>/dev/null || true)
    if [[ -z "${CONFIG_LINE}" ]]; then
        echo -e "\e[1;31mERROR:\e[0m CONFIG_EXTRA_FIRMWARE not found in .config" >&2
        exit 1
    fi

    BLOBS=$(echo "${CONFIG_LINE}" \
        | sed 's/CONFIG_EXTRA_FIRMWARE="\(.*\)"/\1/' \
        | tr ' ' '\n' \
        | grep -v '^$')

    if [[ -z "${BLOBS}" ]]; then
        echo "CONFIG_EXTRA_FIRMWARE is empty — nothing to fetch."
    else
        echo -e "\e[1;34m[*]\e[0m Blobs required by CONFIG_EXTRA_FIRMWARE:"
        echo "${BLOBS}" | sed 's/^/    /'
        echo ""
        mkdir -p "${FIRMWARE_DIR}"
        FAILED=()
        while IFS= read -r blob; do
            dest="${FIRMWARE_DIR}/${blob}"
            if [[ -f "${dest}" ]]; then
                echo -e "  \e[1;32m[=]\e[0m Already exists: ${blob}"
                continue
            fi
            mkdir -p "$(dirname "${dest}")"
            echo -e "  \e[1;34m[↓]\e[0m Fetching: ${blob}"
            if curl -fsSL --retry 3 --retry-delay 2 \
                    "${BASE_URL}/${blob}" -o "${dest}"; then
                echo -e "  \e[1;32m[✓]\e[0m ${blob}"
            else
                echo -e "  \e[1;31m[✗]\e[0m FAILED: ${blob}" >&2
                FAILED+=("${blob}")
                rm -f "${dest}"
            fi
        done <<< "${BLOBS}"

        echo ""
        if [[ ${#FAILED[@]} -eq 0 ]]; then
            echo -e "\e[1;32mAll firmware blobs fetched → ${FIRMWARE_DIR}\e[0m"
        else
            echo -e "\e[1;31mThe following blobs could not be fetched:\e[0m"
            printf '  %s\n' "${FAILED[@]}"
            exit 1
        fi
    fi
fi

if [[ "$DO_BUILD" == "1" ]]; then
    scripts/config --enable CONFIG_LTO_CLANG_THIN
    scripts/config --disable CONFIG_LOCALVERSION_AUTO

    if [[ "$PROFILE" == "server" ]]; then
        echo -e "\e[1;34m[*]\e[0m Applying server profile kernel config..."
        scripts/config --disable CONFIG_SCHED_BORE
        scripts/config --disable CONFIG_CPU_FREQ_GOV_REFLEX
        scripts/config --enable CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE
        scripts/config --enable CONFIG_CPU_FREQ_GOV_PERFORMANCE
        scripts/config --disable CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
        scripts/config --disable CONFIG_CPU_FREQ_GOV_SCHEDUTIL
        scripts/config --enable CONFIG_HZ_250
        scripts/config --set-val CONFIG_HZ 250
        scripts/config --disable CONFIG_PREEMPT
        scripts/config --enable CONFIG_PREEMPT_VOLUNTARY
        scripts/config --disable CONFIG_PREEMPT_NONE
    else
        echo -e "\e[1;34m[*]\e[0m Applying general use profile kernel config..."
        scripts/config --enable CONFIG_SCHED_BORE
        scripts/config --enable CONFIG_CPU_FREQ_GOV_REFLEX
        scripts/config --enable CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
        scripts/config --enable CONFIG_CPU_FREQ_GOV_SCHEDUTIL
        scripts/config --disable CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE
        scripts/config --enable CONFIG_HZ_1000
        scripts/config --set-val CONFIG_HZ 1000
        scripts/config --enable CONFIG_PREEMPT
        scripts/config --disable CONFIG_PREEMPT_VOLUNTARY
        scripts/config --disable CONFIG_PREEMPT_NONE
    fi

    if [[ -d "${FIRMWARE_DIR}" ]] && [[ -n "$(ls -A "${FIRMWARE_DIR}" 2>/dev/null)" ]]; then
        echo -e "\e[1;34m[*]\e[0m Setting CONFIG_EXTRA_FIRMWARE_DIR=${FIRMWARE_DIR}"
        scripts/config --set-str CONFIG_EXTRA_FIRMWARE_DIR "${FIRMWARE_DIR}"
    else
        echo -e "\e[1;33m[!]\e[0m WARNING: extra_firmware/ missing or empty — run fetch firmware first." >&2
    fi

    echo -e "\e[1;34m[*]\e[0m Running olddefconfig..."
    make "${MAKE_OPTS[@]}" olddefconfig

    echo -e "\e[1;34m[*]\e[0m Running prepare..."
    make "${MAKE_OPTS[@]}" prepare

    echo -e "\e[1;34m[*]\e[0m Building bzImage with ${JOBS} jobs..."
    time make "${MAKE_OPTS[@]}" bzImage

    BZIMAGE="arch/x86/boot/bzImage"
    if [[ ! -f "${BZIMAGE}" ]]; then
        echo -e "\e[1;31mERROR:\e[0m bzImage not found after build." >&2
        exit 1
    fi

    mkdir -p "${OUTPUT_DIR}"
    cp "${BZIMAGE}" "${OUTPUT_DIR}/bzImage"
    cp .config "${OUTPUT_DIR}/.config"

    KVER=$(cat include/config/kernel.release 2>/dev/null || echo "unknown")
    echo ""
    echo -e "\e[1;32m╔══════════════════════════════════════════════════╗\e[0m"
    echo -e "\e[1;32m║\e[0m  Build complete!                                 \e[1;32m║\e[0m"
    echo -e "\e[1;32m║\e[0m  Kernel : $(printf "%-39s" "${KVER}")\e[1;32m║\e[0m"
    echo -e "\e[1;32m║\e[0m  bzImage: $(printf "%-39s" "${OUTPUT_DIR}/bzImage")\e[1;32m║\e[0m"
    echo -e "\e[1;32m╚══════════════════════════════════════════════════╝\e[0m"
    echo ""
    echo "Deploy to PS4:"
    echo "  scp ${OUTPUT_DIR}/bzImage root@<ps4-ip>:/boot/bzImage"
fi
