### PS4-Linux Kernel 5.4.247; for Baikal Southbridge

 -- By:
- https://github.com/DFAUS-git/ps4-baikal-5.4.247-kernel
- https://github.com/feeRnt/ps4-linux-12xx
- and many others; check the default branch for the full credits

# How to compile:
=========

```bash
mv config .config

export MAKE_OPTS="-j`nproc` \
              HOSTCC=gcc-11 \
              CC=gcc-11"
# gcc-11 is ideal for compiling the 5.x kernels
# Otherwise you will have many typecheck and compile errors 
make ${MAKE_OPTS} olddefconfig
make ${MAKE_OPTS} prepare
echo "making kernel. . ."
make ${MAKE_OPTS}
echo "making modules . . ."
make ${MAKE_OPTS} modules
# echo "installing kernel . . ."
# make ${MAKE_OPTS} install
# echo "installing modules . . ."
# make ${MAKE_OPTS} modules_install
```

The compiled bzImage will be in arch/x86/boot/bzImage.


Generic Linux kernel documentation
==================================

There are several guides for kernel developers and users. These guides can
be rendered in a number of formats, like HTML and PDF. Please read
Documentation/admin-guide/README.rst first.

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/

There are various text files in the Documentation/ subdirectory,
several of them using the Restructured Text markup notation.

Please read the Documentation/process/changes.rst file, as it contains the
requirements for building and running the kernel, and information about
the problems which may result by upgrading your kernel.
