#!/usr/bin/env bash
# build_bzimage.sh — Local PS4-Linux bzImage builder
# Run from the root of ps4-linux-12xx/ on the 6.18.18-Strawberry branch

set -euo pipefail

# ── Config ────────────────────────────────────────────────────────────────────
OUTPUT_DIR="${PWD}/out"
FIRMWARE_DIR="${PWD}/extra_firmware"
BASE_URL="https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main"

export KCFLAGS="-march=btver2 -mtune=btver2 -O3"
export KAFLAGS="-march=btver2 -mtune=btver2 -O3"
export HOSTCFLAGS="-Wno-error=incompatible-pointer-types-discards-qualifiers"

# ── Interactive UI ─────────────────────────────────────────────────────────────
JOBS=$(nproc)
MAX_JOBS=$(nproc)

while true; do
    clear
    echo -e "\e[1;35m╔══════════════════════════════════════════════════╗\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;37mPS4-Linux Strawberry Builder\e[0m                     \e[1;35m║\e[0m"
    echo -e "\e[1;35m╠══════════════════════════════════════════════════╣\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;32m1)\e[0m Build bzImage                                 \e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;32m2)\e[0m Fetch firmware blobs                          \e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;32m3)\e[0m Both (fetch + build)                          \e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;32m4)\e[0m Threads to use: \e[1;33m$(printf "%-29s" "${JOBS} / ${MAX_JOBS}")\e[0m \e[1;35m║\e[0m"
    echo -e "\e[1;35m║\e[0m \e[1;31m5)\e[0m Quit                                          \e[1;35m║\e[0m"
    echo -e "\e[1;35m╚══════════════════════════════════════════════════╝\e[0m"
    echo ""
    read -p "Select option [1-5]: " CHOICE

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
        *) echo -e "\e[1;31m[!] Invalid option.\e[0m"; sleep 1 ;;
    esac
done

# Initialize make arguments *after* the user has finalized the job count
MAKE_OPTS=(
    -j"${JOBS}"
    LLVM=1
    ARCH=x86_64
    HOSTCFLAGS="${HOSTCFLAGS}"
)

# ── Sanity checks ─────────────────────────────────────────────────────────────
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

# ── Firmware fetch logic ───────────────────────────────────────────────────────
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

# ── Build ─────────────────────────────────────────────────────────────────────
if [[ "$DO_BUILD" == "1" ]]; then
    # Base optimizations
    scripts/config --enable CONFIG_LTO_CLANG_THIN
    scripts/config --disable CONFIG_LOCALVERSION_AUTO

    # Explicitly enable custom features (Polly removed for stability)
    echo -e "\e[1;34m[*]\e[0m Enabling BORE Scheduler, Reflex Governor, and BBR..."
    scripts/config --enable CONFIG_CPU_FREQ_GOV_REFLEX
    scripts/config --enable CONFIG_TCP_CONG_BBR
    scripts/config --enable CONFIG_SCHED_BORE

    # Point firmware dir at local extra_firmware/
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