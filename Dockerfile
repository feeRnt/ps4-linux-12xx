# State an OS to use with from
# https://hub.docker.com/search?categories=Operating+systems
FROM ubuntu:22.04 AS build-env

# Inherit everything from this first build stage onto this second stage:
# We can have all the commands in one single stage/directive, but this is cleaner and useful

FROM build-env AS deb-src

#You can use both "EOF" and EOF in this first heredoc declaration
RUN cat <<"EOF" > /etc/apt/sources.list
deb http://archive.ubuntu.com/ubuntu/ jammy main restricted universe multiverse
deb-src http://archive.ubuntu.com/ubuntu/ jammy main restricted universe multiverse

deb http://archive.ubuntu.com/ubuntu/ jammy-updates main restricted universe multiverse
deb-src http://archive.ubuntu.com/ubuntu/ jammy-updates main restricted universe multiverse

deb http://archive.ubuntu.com/ubuntu/ jammy-security main restricted universe multiverse
deb-src http://archive.ubuntu.com/ubuntu/ jammy-security main restricted universe multiverse

deb http://archive.ubuntu.com/ubuntu/ jammy-backports main restricted universe multiverse
deb-src http://archive.ubuntu.com/ubuntu/ jammy-backports main restricted universe multiverse

# deb http://archive.canonical.com/ubuntu/ jammy partner
# deb-src http://archive.canonical.com/ubuntu/ jammy partner
EOF
# List copied from 
# https://gist.github.com/hakerdefo/9c99e140f543b5089e32176fe8721f5f

FROM deb-src AS install-deps

ARG DEBIAN_FRONTEND=noninteractive
# Used only at build time
ENV TZ=Etc/UTC
# Used both at build and runtime
# Need non interactive builds and installs and TZ set for package "tzdata"

RUN <<"EOF"
#DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get update did not work.
apt-get update
apt-get install build-essential wget git -y
apt-get build-dep linux -y
EOF

FROM install-deps AS install-deps2

RUN DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get install gcc-11 libgcc-11-dev libssl-dev -y 

#openssl2.1-dev doesn't exist
#OpenSSL 2.1 needed for Linux kernel version 5.15.
#Without gcc-11 (or older), you will probably get compilation errors

FROM install-deps2 AS install-extra-firmware

RUN <<"EOF"
mkdir -p /lib/firmware/mrvl
cd /lib/firmware/mrvl
wget -nc https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mrvl/sd8897_uapsta.bin
wget -nc https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mrvl/pcie8897_uapsta.bin
wget -nc https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mrvl/sd8797_uapsta.bin

mkdir -p /lib/firmware/mediatek
cd /lib/firmware/mediatek
wget -nc https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek/mt7668pr2h.bin

#### Extra firmware that are / might be needed for kernel compilation. 
EOF
# Wget code snippet from https://github.com/TigerClips1

# Clone the Linux kernel source . . . This fails to cache as expected.
# A slight change of git clone invalidates the whole kernel source
# directory cache. So used custom caching mecahnism in github.yaml
FROM install-extra-firmware AS prepare-kernel-source

RUN mkdir -p /container/workspace/kernel-source-container
WORKDIR /container/workspace/kernel-source-container

# This step is no longer necessary as we are going to compile outside of the docker image build. (Outside of Dockerfile).
###FROM prepare-kernel-source AS compile-kernel

# Dockerfile adapted from 
#https://moebuta.org/posts/using-github-actions-to-build-linux-kernels/
