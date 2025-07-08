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

FROM deb-src AS install-deps

ARG DEBIAN_FRONTEND=noninteractive
#only at build time

ENV TZ=Etc/UTC
#both at build and runtime


RUN <<"EOF"
#DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get update did not work.
#Probably because vars were needed for apt-get install build-essential
apt-get update
apt-get install build-essential wget git -y
apt-get build-dep linux -y
EOF

# Need non interactive builds and installs and TZ set for package "tzdata"

FROM install-deps AS install-deps2

RUN DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get install gcc-11 libgcc-11-dev libssl-dev -y #openssl2.1-dev doesn't exist

#OpenSSL 2.1 needed for Linux kernel version 5.15.
#Without gcc-11 (or older), you will probably get compilation errors


# Clone the Linux kernel source
FROM install-deps2 AS clone-kernel-source

#WORKDIR /kernel-source
#RUN git clone -b "ps4-linux-5.15.y-conservative2" --depth=1 https://github.com/feeRnt/ps4-linux-12xx.git

# we don't need to clone anything as checkout in the github yaml action already takes care of that

WORKDIR /kernel-source
COPY . .
# Copy everything (except the .dockerignore files) in the main github repository /./ = / to
# . = /kernel-source inside of the docker image. 
# syntax: COPY local-public-path-relative-to-Dockerfile remote-private-path-inside-of-Dockerimage

# Compile the Linux kernel
FROM clone-kernel-source AS compile-kernel

RUN <<"EOF"
set -e  
# Exit immediately if any command fails
export BRANCH=`git rev-parse --abbrev-ref HEAD | sed s/-/+/g`
export SHA1=`git rev-parse --short HEAD`
export LOCALVERSION=+${BRANCH}+${SHA1}+GCE
export GCE_PKG_DIR=${PWD}/gce/${LOCALVERSION}/pkg
export GCE_INSTALL_DIR=${PWD}/gce/${LOCALVERSION}/install
export GCE_BUILD_DIR=${PWD}/gce/${LOCALVERSION}/build
export KERNEL_PKG=kernel-${LOCALVERSION}.tar.gz2
export MAKE_OPTS="-j`nproc` \
           INSTALL_PATH=${GCE_INSTALL_DIR}/boot \
           INSTALL_MOD_PATH=${GCE_INSTALL_DIR} \
	   HOSTCC=gcc-11 \
	   CC=gcc-11"
mkdir -p ${GCE_BUILD_DIR}
mkdir -p ${GCE_INSTALL_DIR}/boot
mkdir -p ${GCE_PKG_DIR}
make ${MAKE_OPTS} olddefconfig
make ${MAKE_OPTS} prepare
make ${MAKE_OPTS}
make ${MAKE_OPTS} modules
make ${MAKE_OPTS} install
make ${MAKE_OPTS} modules_install
cd ${GCE_INSTALL_DIR}
tar -cvzf /kernel.tar.gz2 boot/* lib/modules/* --owner=0 --group=0
EOF

## GCE = Google Compute Engine, adapted from 
#https://github.com/google/bbr/blob/v3/gce-install.sh

# Dockerfile adapted from 
#https://moebuta.org/posts/using-github-actions-to-build-linux-kernels/
