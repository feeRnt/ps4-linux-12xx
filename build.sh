#!/usr/bin/env bash

# PS4-Linux Strawberry Builder
# Supports two PS4-focused build profiles and two LTO flavors:
#   server  — headless/services, HZ=250, PREEMPT_VOLUNTARY, performance governor
#   general — desktop/gaming, HZ=250, PREEMPT=y, BORE, schedutil/reflex
#   ThinLTO / FullLTO selectable via lto=ThinLTO or lto=FullLTO
#
# Usage:
#   ./build.sh
#   ./build.sh --option N
#   ./build.sh --option N use=Server
#   ./build.sh --option N lto=ThinLTO
#   ./build.sh --option N use=General lto=FullLTO
#   ./build.sh --option 7             Show/switch build profile
#   ./build.sh --option 8             Show/switch LTO flavor

set -euo pipefail

OUTPUT_DIR="${PWD}/out"
FIRMWARE_DIR="${PWD}/extra_firmware"
BASE_URL="https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main"

# PS4 Jaguar tuning
export KCFLAGS="-march=btver2 -mtune=btver2 -Os"
export KAFLAGS="-march=btver2 -mtune=btver2 -Os"
export HOSTCFLAGS="-Wno-error=incompatible-pointer-types-discards-qualifiers"

PROFILE="server"
LTO_FLAVOR="thin"
JOBS="$(nproc)"
MAX_JOBS="$(nproc)"

lto_label() {
    if [[ "$LTO_FLAVOR" == "full" ]]; then
        echo "FullLTO"
    else
        echo "ThinLTO"
    fi
}

# Parse optional selectors in any position:
#   use=Server/use=General
#   lto=ThinLTO/lto=FullLTO
for arg in "$@"; do
    case "$arg" in
        use=*)
            PROFILE_ARG="${arg#use=}"
            case "${PROFILE_ARG,,}" in
                server) PROFILE="server" ;;
                general) PROFILE="general" ;;
                *)
                    echo "Unknown build profile: ${PROFILE_ARG}. Valid: Server, General"
                    exit 1
                    ;;
            esac
            ;;
        lto=*)
            LTO_ARG="${arg#lto=}"
            case "${LTO_ARG,,}" in
                thinlto|thin) LTO_FLAVOR="thin" ;;
                fulllto|full) LTO_FLAVOR="full" ;;
                *)
                    echo "Unknown LTO flavor: ${LTO_ARG}. Valid: ThinLTO, FullLTO"
                    exit 1
                    ;;
            esac
            ;;
    esac
done

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
            read -r -p "Switch profile? (y/n): " SWITCH
            if [[ "$SWITCH" =~ ^[Yy]$ ]]; then
                [[ "$PROFILE" == "server" ]] && PROFILE="general" || PROFILE="server"
                echo "Profile switched to: ${PROFILE}"
            fi
            exit 0
            ;;
        8)
            echo "Current LTO flavor: $(lto_label)"
            read -r -p "Switch LTO flavor? (y/n): " SWITCH
            if [[ "$SWITCH" =~ ^[Yy]$ ]]; then
                [[ "$LTO_FLAVOR" == "thin" ]] && LTO_FLAVOR="full" || LTO_FLAVOR="thin"
                echo "LTO flavor switched to: $(lto_label)"
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
        echo -e "\e[1;35m║\e[0m \e[1;31m5)\e[0m Quit                                          \e[1;35m║\e[0m"
        echo -e "\e[1;35m║\e[0m \e[1;32m6)\e[0m Build profile: \e[1;33m${PROFILE}\e[0m$(printf "%-22s" "")\e[1;35m║\e[0m"
        echo -e "\e[1;35m║\e[0m \e[1;36m7)\e[0m Show/switch build profile                      \e[1;35m║\e[0m"
        echo -e "\e[1;35m║\e[0m \e[1;32m8)\e[0m Build LTO: \e[1;33m$(printf "%-31s" "$(lto_label)")\e[0m \e[1;35m║\e[0m"
        echo -e "\e[1;35m╚══════════════════════════════════════════════════╝\e[0m"
        echo ""
        read -r -p "Select option [1-8]: " CHOICE

        case "$CHOICE" in
            1) DO_BUILD=1; DO_FETCH=0; break ;;
            2) DO_BUILD=0; DO_FETCH=1; break ;;
            3) DO_BUILD=1; DO_FETCH=1; break ;;
            4)
                read -r -p "Enter number of threads (1-${MAX_JOBS}): " NEW_JOBS
                if [[ "$NEW_JOBS" =~ ^[0-9]+$ ]] && [[ "$NEW_JOBS" -ge 1 ]] && [[ "$NEW_JOBS" -le "$MAX_JOBS" ]]; then
                    JOBS="$NEW_JOBS"
                else
                    echo -e "\e[1;31m[!] Invalid input.\e[0m Press enter to continue."
                    read -r
                fi
                ;;
            5)
                echo "Exiting."
                exit 0
                ;;
            6)
                echo ""
                echo "Select build profile:"
                echo "  1) Server  (max throughput, headless)"
                echo "  2) General (gaming/desktop latency)"
                read -r -p "Profile [1-2]: " PROFILE_CHOICE
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
                read -r -p "Switch profile? (y/n): " SWITCH
                if [[ "$SWITCH" =~ ^[Yy]$ ]]; then
                    [[ "$PROFILE" == "server" ]] && PROFILE="general" || PROFILE="server"
                    echo "Profile switched to: ${PROFILE}"
                    sleep 1
                fi
                ;;
            8)
                echo ""
                echo "Current LTO flavor: $(lto_label)"
                read -r -p "Switch LTO flavor? (y/n): " SWITCH
                if [[ "$SWITCH" =~ ^[Yy]$ ]]; then
                    [[ "$LTO_FLAVOR" == "thin" ]] && LTO_FLAVOR="full" || LTO_FLAVOR="thin"
                    echo "LTO flavor switched to: $(lto_label)"
                    sleep 1
                fi
                ;;
            *)
                echo -e "\e[1;31m[!] Invalid option.\e[0m"
                sleep 1
                ;;
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
    echo -e "\e[1;31mERROR:\e[0m Run this from the kernel source root." >&2
    exit 1
fi

if [[ ! -f .config ]]; then
    if [[ -f config ]]; then
        echo -e "\e[1;34m[*]\e[0m Moving 'config' -> '.config'"
        mv config .config
    else
        echo -e "\e[1;31mERROR:\e[0m No .config found." >&2
        exit 1
    fi
fi

if [[ "$DO_FETCH" == "1" ]]; then
    CONFIG_LINE="$(grep -E '^CONFIG_EXTRA_FIRMWARE=' .config 2>/dev/null || true)"
    if [[ -z "${CONFIG_LINE}" ]]; then
        echo -e "\e[1;31mERROR:\e[0m CONFIG_EXTRA_FIRMWARE not found in .config" >&2
        exit 1
    fi

    BLOBS="$(echo "${CONFIG_LINE}" | sed 's/CONFIG_EXTRA_FIRMWARE="\(.*\)"/\1/' | tr ' ' '\n' | grep -v '^$' || true)"

    if [[ -z "${BLOBS}" ]]; then
        echo "CONFIG_EXTRA_FIRMWARE is empty -- nothing to fetch."
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
            if curl -fsSL --retry 3 --retry-delay 2 "${BASE_URL}/${blob}" -o "${dest}"; then
                echo -e "  \e[1;32m[✓]\e[0m ${blob}"
            else
                echo -e "  \e[1;31m[✗]\e[0m FAILED: ${blob}" >&2
                FAILED+=("${blob}")
                rm -f "${dest}"
            fi
        done <<< "${BLOBS}"

        echo ""
        if [[ ${#FAILED[@]} -eq 0 ]]; then
            echo -e "\e[1;32mAll firmware blobs fetched -> ${FIRMWARE_DIR}\e[0m"
        else
            echo -e "\e[1;31mThe following blobs could not be fetched:\e[0m"
            printf '  %s\n' "${FAILED[@]}"
            exit 1
        fi
    fi
fi

if [[ "$DO_BUILD" == "1" ]]; then
    echo -e "\e[1;34m[*]\e[0m Applying invariant config..."

    LOCALVERSION_SUFFIX="-Strawberry-$(lto_label)-"

    # Build system / LTO
    if [[ "$LTO_FLAVOR" == "full" ]]; then
        echo -e "\e[1;34m[*]\e[0m Enabling FullLTO..."
        scripts/config --disable CONFIG_LTO_CLANG_THIN
        scripts/config --enable  CONFIG_LTO_CLANG_FULL
    else
        echo -e "\e[1;34m[*]\e[0m Enabling ThinLTO..."
        scripts/config --enable  CONFIG_LTO_CLANG_THIN
        scripts/config --disable CONFIG_LTO_CLANG_FULL
    fi
    scripts/config --disable CONFIG_LOCALVERSION_AUTO
    scripts/config --set-str CONFIG_LOCALVERSION "${LOCALVERSION_SUFFIX}"

    # Kernel compression
    scripts/config --disable CONFIG_KERNEL_XZ
    scripts/config --enable  CONFIG_KERNEL_ZSTD

    # NUMA removal
    scripts/config --disable CONFIG_NUMA
    scripts/config --disable CONFIG_AMD_NUMA
    scripts/config --disable CONFIG_X86_64_ACPI_NUMA
    scripts/config --disable CONFIG_ACPI_NUMA
    scripts/config --disable CONFIG_NUMA_MEMBLKS
    scripts/config --disable CONFIG_NUMA_BALANCING

    # Bare-metal PS4 target
    scripts/config --disable CONFIG_HYPERVISOR_GUEST
    scripts/config --disable CONFIG_PARAVIRT
    scripts/config --disable CONFIG_PARAVIRT_XXL
    scripts/config --disable CONFIG_KVM
    scripts/config --disable CONFIG_KVM_AMD
    scripts/config --disable CONFIG_KVM_INTEL

    # PS4 firmware
    scripts/config --enable  CONFIG_PS4_DMI_SPOOF

    # Memory management / cgroup base
    scripts/config --enable  CONFIG_CGROUPS
    scripts/config --enable  CONFIG_MEMCG
    scripts/config --enable  CONFIG_BLK_CGROUP
    scripts/config --enable  CONFIG_CGROUP_WRITEBACK
    scripts/config --enable  CONFIG_CGROUP_SCHED
    scripts/config --enable  CONFIG_FAIR_GROUP_SCHED
    scripts/config --disable CONFIG_RT_GROUP_SCHED
    scripts/config --enable  CONFIG_CFS_BANDWIDTH
    scripts/config --enable  CONFIG_CGROUP_PIDS
    scripts/config --enable  CONFIG_LRU_GEN
    scripts/config --enable  CONFIG_LRU_GEN_ENABLED
    scripts/config --enable  CONFIG_LRU_GEN_STATS
    scripts/config --enable  CONFIG_TRANSPARENT_HUGEPAGE
    scripts/config --enable  CONFIG_SLUB_CPU_PARTIAL
    scripts/config --enable  CONFIG_CGROUP_DMEM
    scripts/config --enable  CONFIG_CGROUP_FREEZER
    scripts/config --enable  CONFIG_CPUSETS
    scripts/config --enable  CONFIG_CGROUP_DEVICE
    scripts/config --enable  CONFIG_CGROUP_CPUACCT
    scripts/config --enable  CONFIG_CGROUP_MISC
    scripts/config --enable  CONFIG_CGROUP_BPF

    # Namespace support needed by most cgroup/container userspace
    scripts/config --enable  CONFIG_NAMESPACES
    scripts/config --enable  CONFIG_UTS_NS
    scripts/config --enable  CONFIG_TIME_NS
    scripts/config --enable  CONFIG_IPC_NS
    scripts/config --enable  CONFIG_USER_NS
    scripts/config --enable  CONFIG_PID_NS
    scripts/config --enable  CONFIG_NET_NS

    scripts/config --disable CONFIG_ZSWAP_COMPRESSOR_DEFAULT_LZO
    scripts/config --enable  CONFIG_ZSWAP_COMPRESSOR_DEFAULT_ZSTD
    scripts/config --set-str CONFIG_ZSWAP_COMPRESSOR_DEFAULT "zstd"
    scripts/config --disable CONFIG_ZRAM_DEF_COMP_LZ4
    scripts/config --enable  CONFIG_ZRAM_DEF_COMP_ZSTD
    scripts/config --set-str CONFIG_ZRAM_DEF_COMP "zstd"
    scripts/config --enable  CONFIG_ZSWAP
    scripts/config --enable  CONFIG_ZRAM

    # Async I/O
    scripts/config --enable  CONFIG_IO_URING

    # Network
    scripts/config --enable  CONFIG_TCP_CONG_BBR
    scripts/config --set-str CONFIG_DEFAULT_TCP_CONG "bbr"
    scripts/config --enable  CONFIG_NET_SCH_DEFAULT
    scripts/config --enable  CONFIG_NET_SCH_FQ
    scripts/config --enable  CONFIG_NET_SCH_FQ_CODEL
    scripts/config --enable  CONFIG_NET_SCH_CAKE

    # Crypto acceleration
    scripts/config --enable  CONFIG_CRYPTO_AES_NI_INTEL
    scripts/config --enable  CONFIG_CRYPTO_GHASH_CLMUL_NI_INTEL
    scripts/config --enable  CONFIG_CRYPTO_POLYVAL_CLMUL_NI
    scripts/config --enable  CONFIG_CRYPTO_LIB_SHA256

    # Futex
    scripts/config --enable  CONFIG_FUTEX
    scripts/config --enable  CONFIG_FUTEX_PI
    scripts/config --enable  CONFIG_FUTEX_PRIVATE_HASH
    scripts/config --enable  CONFIG_FUTEX_MPOL

    # NTSYNC
    scripts/config --enable  CONFIG_NTSYNC

    # Scheduler
    scripts/config --enable  CONFIG_SCHED_CLASS_EXT
    scripts/config --enable  CONFIG_SCHED_EXT
    scripts/config --enable  CONFIG_SCHED_AUTOGROUP

    # BPF
    scripts/config --enable  CONFIG_BPF_SYSCALL
    scripts/config --enable  CONFIG_BPF_JIT
    scripts/config --enable  CONFIG_BPF_JIT_ALWAYS_ON
    scripts/config --enable  CONFIG_BPF_JIT_DEFAULT_ON
    scripts/config --disable CONFIG_BPF_UNPRIV_DEFAULT_OFF

    # BTF / debug metadata
    scripts/config --enable  CONFIG_DEBUG_INFO
    scripts/config --enable  CONFIG_DEBUG_INFO_DWARF4
    scripts/config --disable CONFIG_DEBUG_INFO_DWARF5
    scripts/config --disable CONFIG_DEBUG_INFO_REDUCED
    scripts/config --disable CONFIG_DEBUG_INFO_SPLIT
    scripts/config --enable  CONFIG_DEBUG_INFO_BTF
    scripts/config --enable  CONFIG_DEBUG_INFO_BTF_MODULES

    # Runtime debug / tracing
    scripts/config --disable CONFIG_DEBUG_KERNEL
    scripts/config --disable CONFIG_PROVE_LOCKING
    scripts/config --disable CONFIG_LOCKDEP
    scripts/config --disable CONFIG_KASAN
    scripts/config --disable CONFIG_FTRACE
    scripts/config --disable CONFIG_SCHED_DEBUG
    scripts/config --disable CONFIG_DEBUG_FS

    # Mitigation / hardening trims
    scripts/config --disable CONFIG_CPU_MITIGATIONS
    scripts/config --disable CONFIG_STACKPROTECTOR
    scripts/config --disable CONFIG_STACKPROTECTOR_STRONG
    scripts/config --disable CONFIG_RANDOMIZE_KSTACK_OFFSET_DEFAULT
    scripts/config --disable CONFIG_SLAB_FREELIST_HARDENED
    scripts/config --disable CONFIG_SLAB_FREELIST_RANDOM
    scripts/config --disable CONFIG_SHUFFLE_PAGE_ALLOCATOR
    scripts/config --disable CONFIG_INIT_ON_ALLOC_DEFAULT_ON
    scripts/config --disable CONFIG_INIT_ON_FREE_DEFAULT_ON
    scripts/config --disable CONFIG_FORTIFY_SOURCE
    scripts/config --disable CONFIG_HARDENED_USERCOPY
    scripts/config --disable CONFIG_HARDENED_USERCOPY_DEFAULT_ON
    scripts/config --disable CONFIG_SECURITY_DMESG_RESTRICT
    scripts/config --disable CONFIG_IOMMU_DEFAULT_DMA_STRICT
    scripts/config --enable  CONFIG_IOMMU_DEFAULT_DMA_LAZY

    # I/O schedulers
    scripts/config --enable  CONFIG_MQ_IOSCHED_DEADLINE
    scripts/config --enable  CONFIG_MQ_IOSCHED_KYBER
    scripts/config --enable  CONFIG_IOSCHED_BFQ
    scripts/config --enable  CONFIG_BFQ_GROUP_IOSCHED
    scripts/config --enable  CONFIG_BLK_WBT
    scripts/config --enable  CONFIG_BLK_WBT_MQ

    # Strip debug overhead
    scripts/config --disable CONFIG_DMADEVICES_DEBUG
    scripts/config --disable CONFIG_DMADEVICES_VDEBUG
    scripts/config --disable CONFIG_IOMMU_DEBUG
    scripts/config --disable CONFIG_I2C_DEBUG_CORE
    scripts/config --disable CONFIG_I2C_DEBUG_ALGO
    scripts/config --disable CONFIG_I2C_DEBUG_BUS
    scripts/config --disable CONFIG_DM_DEBUG
    scripts/config --disable CONFIG_BLK_DEBUG_FS

    if [[ "$PROFILE" == "server" ]]; then
        echo -e "\e[1;34m[*]\e[0m Applying server profile..."

        scripts/config --disable CONFIG_SCHED_BORE
        scripts/config --disable CONFIG_SCHED_AUTOGROUP
        scripts/config --disable CONFIG_CPU_FREQ_GOV_REFLEX

        scripts/config --disable CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
        scripts/config --disable CONFIG_CPU_FREQ_GOV_SCHEDUTIL
        scripts/config --enable  CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE
        scripts/config --enable  CONFIG_CPU_FREQ_GOV_PERFORMANCE

        scripts/config --disable CONFIG_HZ_1000
        scripts/config --disable CONFIG_HZ_300
        scripts/config --disable CONFIG_HZ_100
        scripts/config --enable  CONFIG_HZ_250
        scripts/config --set-val CONFIG_HZ 250

        scripts/config --enable  CONFIG_NO_HZ_IDLE
        scripts/config --disable CONFIG_NO_HZ_FULL

        scripts/config --disable CONFIG_PREEMPT
        scripts/config --enable  CONFIG_PREEMPT_VOLUNTARY
        scripts/config --disable CONFIG_PREEMPT_NONE

        scripts/config --enable  CONFIG_PSI
        scripts/config --enable  CONFIG_PSI_DEFAULT_DISABLED

        scripts/config --enable  CONFIG_TRANSPARENT_HUGEPAGE
        scripts/config --disable CONFIG_TRANSPARENT_HUGEPAGE_ALWAYS
        scripts/config --enable  CONFIG_TRANSPARENT_HUGEPAGE_MADVISE

        scripts/config --enable  CONFIG_NETFILTER
        scripts/config --enable  CONFIG_NETFILTER_ADVANCED
        scripts/config --enable  CONFIG_NETFILTER_XTABLES
        scripts/config --enable  CONFIG_NF_TABLES
        scripts/config --enable  CONFIG_NF_TABLES_INET
        scripts/config --enable  CONFIG_NF_TABLES_IPV4
        scripts/config --enable  CONFIG_NF_TABLES_IPV6
        scripts/config --enable  CONFIG_IP_NF_IPTABLES
        scripts/config --enable  CONFIG_IP6_NF_IPTABLES
        scripts/config --enable  CONFIG_BRIDGE
        scripts/config --enable  CONFIG_BRIDGE_NETFILTER
        scripts/config --enable  CONFIG_VETH
        scripts/config --enable  CONFIG_OVERLAY_FS

        scripts/config --set-str CONFIG_DEFAULT_IOSCHED "mq-deadline"

        scripts/config --enable  CONFIG_DEFAULT_FQ
        scripts/config --disable CONFIG_DEFAULT_FQ_CODEL
        scripts/config --disable CONFIG_DEFAULT_FQ_PIE
        scripts/config --disable CONFIG_DEFAULT_SFQ
        scripts/config --disable CONFIG_DEFAULT_PFIFO_FAST
        scripts/config --set-str CONFIG_DEFAULT_NET_SCH "fq"
    else
        echo -e "\e[1;34m[*]\e[0m Applying general/gaming profile..."

        scripts/config --disable CONFIG_CPU_MITIGATIONS

        # Desktop userspace compatibility
        scripts/config --enable  CONFIG_DMIID
        scripts/config --enable  CONFIG_DMI_SYSFS
        scripts/config --enable  CONFIG_FW_CFG_SYSFS

        scripts/config --enable  CONFIG_SCHED_BORE
        scripts/config --set-val CONFIG_MIN_BASE_SLICE_NS 2000000

        scripts/config --enable  CONFIG_CPU_FREQ_GOV_REFLEX
        scripts/config --enable  CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
        scripts/config --enable  CONFIG_CPU_FREQ_GOV_SCHEDUTIL
        scripts/config --disable CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE

        scripts/config --enable  CONFIG_HZ_250
        scripts/config --disable CONFIG_HZ_300
        scripts/config --disable CONFIG_HZ_100
        scripts/config --disable CONFIG_HZ_1000
        scripts/config --set-val CONFIG_HZ 250
        scripts/config --enable  CONFIG_NO_HZ_IDLE
        scripts/config --enable  CONFIG_NO_HZ_FULL
        scripts/config --enable  CONFIG_RCU_NOCB_CPU
        scripts/config --enable  CONFIG_RCU_NOCB_CPU_DEFAULT_ALL

        scripts/config --enable  CONFIG_PREEMPT
        scripts/config --disable CONFIG_PREEMPT_VOLUNTARY
        scripts/config --disable CONFIG_PREEMPT_NONE

        scripts/config --enable  CONFIG_TRANSPARENT_HUGEPAGE
        scripts/config --disable CONFIG_TRANSPARENT_HUGEPAGE_ALWAYS
        scripts/config --enable  CONFIG_TRANSPARENT_HUGEPAGE_MADVISE

        scripts/config --disable CONFIG_OVERLAY_FS
        scripts/config --disable CONFIG_VETH
        scripts/config --disable CONFIG_BRIDGE_NETFILTER
        scripts/config --disable CONFIG_BRIDGE
        scripts/config --disable CONFIG_IP6_NF_IPTABLES
        scripts/config --disable CONFIG_IP_NF_IPTABLES
        scripts/config --disable CONFIG_NF_TABLES_IPV6
        scripts/config --disable CONFIG_NF_TABLES_IPV4
        scripts/config --disable CONFIG_NF_TABLES_INET
        scripts/config --disable CONFIG_NF_TABLES
        scripts/config --disable CONFIG_NETFILTER_XTABLES
        scripts/config --disable CONFIG_NETFILTER_ADVANCED
        scripts/config --disable CONFIG_NETFILTER

        scripts/config --disable CONFIG_PSI
        scripts/config --set-str CONFIG_DEFAULT_IOSCHED "bfq"

        scripts/config --enable  CONFIG_DEFAULT_FQ_CODEL
        scripts/config --disable CONFIG_DEFAULT_FQ
        scripts/config --disable CONFIG_DEFAULT_FQ_PIE
        scripts/config --disable CONFIG_DEFAULT_SFQ
        scripts/config --disable CONFIG_DEFAULT_PFIFO_FAST
        scripts/config --set-str CONFIG_DEFAULT_NET_SCH "fq_codel"
    fi

    if [[ -d "${FIRMWARE_DIR}" ]] && [[ -n "$(ls -A "${FIRMWARE_DIR}" 2>/dev/null)" ]]; then
        echo -e "\e[1;34m[*]\e[0m Setting CONFIG_EXTRA_FIRMWARE_DIR=${FIRMWARE_DIR}"
        scripts/config --set-str CONFIG_EXTRA_FIRMWARE_DIR "${FIRMWARE_DIR}"
    else
        echo -e "\e[1;33m[!]\e[0m WARNING: extra_firmware/ missing or empty -- run fetch firmware first." >&2
    fi

    echo -e "\e[1;34m[*]\e[0m Running olddefconfig..."
    make "${MAKE_OPTS[@]}" olddefconfig

    echo -e "\e[1;34m[*]\e[0m Running prepare..."
    make "${MAKE_OPTS[@]}" prepare

    CURRENT_LTO_LABEL="$(lto_label)"
    echo -e "\e[1;34m[*]\e[0m Building bzImage [profile: ${PROFILE}, LTO: ${CURRENT_LTO_LABEL}] with ${JOBS} jobs..."
    time make "${MAKE_OPTS[@]}" bzImage

    BZIMAGE="arch/x86/boot/bzImage"
    if [[ ! -f "${BZIMAGE}" ]]; then
        echo -e "\e[1;31mERROR:\e[0m bzImage not found after build." >&2
        exit 1
    fi

    mkdir -p "${OUTPUT_DIR}"
    cp "${BZIMAGE}" "${OUTPUT_DIR}/bzImage"
    cp .config "${OUTPUT_DIR}/.config"

    KVER="$(cat include/config/kernel.release 2>/dev/null || echo "unknown")"
    LTO_LABEL="${CURRENT_LTO_LABEL}"

    PROFILE_LABEL="Server"
    if [[ "$PROFILE" == "general" ]]; then
        PROFILE_LABEL="General"
    fi

    KVER_BASE="${KVER%%-*}"
    RELEASE_TRACK="Mainline"
    if [[ "$KVER_BASE" == 6.18.* ]]; then
        RELEASE_TRACK="LTS"
    fi

    ARTIFACT_BASENAME="Strawberry-${LTO_LABEL}-${PROFILE_LABEL}-${RELEASE_TRACK}-${KVER}"
    printf '%s\n' "${ARTIFACT_BASENAME}" > "${OUTPUT_DIR}/artifact_name.txt"

    echo ""
    echo -e "\e[1;32m╔══════════════════════════════════════════════════╗\e[0m"
    echo -e "\e[1;32m║\e[0m  Build complete! [${PROFILE} / ${LTO_LABEL}]$(printf "%-13s" "")\e[1;32m║\e[0m"
    echo -e "\e[1;32m║\e[0m  Kernel : $(printf "%-39s" "${KVER}")\e[1;32m║\e[0m"
    echo -e "\e[1;32m║\e[0m  bzImage: $(printf "%-39s" "${OUTPUT_DIR}/bzImage")\e[1;32m║\e[0m"
    echo -e "\e[1;32m╚══════════════════════════════════════════════════╝\e[0m"
    echo ""

    if [[ "$PROFILE" == "general" ]]; then
        echo "Kernel cmdline (add to your kexec invocation):"
        echo "  isolcpus=2-7 nohz_full=2-7 rcu_nocbs=2-7 irqaffinity=0-1 threadirqs"
        echo ""
        echo "Post-boot sysctl (add to /etc/sysctl.d/99-ps4-gaming.conf):"
        echo "  vm.swappiness = 10"
        echo "  vm.dirty_ratio = 15"
        echo "  vm.dirty_background_ratio = 5"
        echo "  vm.compaction_proactiveness = 1"
        echo ""
        echo "Force GPU to max SCLK:"
        echo "  echo manual > /sys/class/drm/card0/device/power_dpm_force_performance_level"
        echo "  echo 2      > /sys/class/drm/card0/device/pp_dpm_sclk"
        echo ""
    fi

    echo "Deploy to PS4:"
    echo "  scp ${OUTPUT_DIR}/bzImage root@<ps4-ip>:/boot/bzImage"
fi
