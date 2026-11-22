-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA512

Format: 3.0 (quilt)
Source: mesa
Binary: libgbm1, libgbm-dev, libegl-mesa0, libegl1-mesa-dev, libgles2-mesa-dev, libglx-mesa0, libgl1-mesa-dri, libgl1-mesa-dev, mesa-common-dev, mesa-libgallium, mesa-teflon-delegate, mesa-va-drivers, mesa-vdpau-drivers, mesa-vulkan-drivers, mesa-opencl-icd, mesa-drm-shim
Architecture: any
Version: 25.2.8-0ubuntu0.24.04.5
Maintainer: Ubuntu Developers <ubuntu-devel-discuss@lists.ubuntu.com>
Uploaders: Andreas Boll <aboll@debian.org>
Homepage: https://mesa3d.org/
Standards-Version: 4.1.4
Vcs-Browser: https://salsa.debian.org/xorg-team/lib/mesa
Vcs-Git: https://salsa.debian.org/xorg-team/lib/mesa.git
Testsuite: autopkgtest
Testsuite-Triggers: build-essential, pkg-config
Build-Depends: debhelper-compat (= 13), directx-headers-dev (>= 1.614.1) [linux-amd64 linux-arm64], flatbuffers-compiler [linux-arm64], glslang-tools [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32], spirv-tools (>= 2025.1~rc1) [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32], meson-1.7 (>= 1.7.0), pkgconf, libdrm-dev (>= 2.4.125-1), libx11-dev, libxxf86vm-dev, libexpat1-dev, libflatbuffers-dev [linux-arm64], libsensors-dev [!hurd-any], libxext-dev, libva-dev (>= 1.6.0) [linux-any] <!pkg.mesa.nolibva>, libvdpau-dev (>= 1.5) [linux-any], libvulkan-dev [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32], x11proto-dev, linux-libc-dev (>= 2.6.31) [linux-any], libx11-xcb-dev, libxcb-dri2-0-dev (>= 1.8), libxcb-glx0-dev (>= 1.8.1), libxcb-dri3-dev, libxcb-present-dev, libxcb-randr0-dev, libxcb-shm0-dev, libxcb-sync-dev, libxrandr-dev, libxshmfence-dev (>= 1.1), libxtensor-dev [linux-arm64], libzstd-dev, python3, python3-mako, python3-yaml, python3-pycparser [arm64 armhf], python3-setuptools, flex, bison, libelf-dev [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32], libwayland-dev (>= 1.15.0) [linux-any], libwayland-egl-backend-dev (>= 1.15.0) [linux-any], llvm-20-dev [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32], libclang-20-dev [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32], libclang-cpp20-dev [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32], libclc-20-dev [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32], libclc-20 [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32], wayland-protocols (>= 1.41), zlib1g-dev, libglvnd-core-dev (>= 1.3.2), valgrind [amd64 arm64 armhf i386 mips64el powerpc ppc64 ppc64el s390x], rustc-1.78 (>= 1.78) [amd64 arm64 armel armhf loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x x32], rustfmt-1.78 [amd64 arm64 armel armhf loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x x32], bindgen-0.71 (>= 0.71~) [amd64 arm64 armel armhf loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x x32], cbindgen [amd64 arm64 armel armhf loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x x32], llvm-spirv-20 [amd64 arm64 armel armhf loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x x32], libllvmspirvlib-20-dev [amd64 arm64 armel armhf i386 loong64 mips64el powerpc ppc64 ppc64el riscv64 s390x sparc64 x32]
Package-List:
 libegl-mesa0 deb libs optional arch=any
 libegl1-mesa-dev deb libdevel optional arch=any
 libgbm-dev deb libdevel optional arch=linux-any
 libgbm1 deb libs optional arch=linux-any
 libgl1-mesa-dev deb oldlibs optional arch=any
 libgl1-mesa-dri deb libs optional arch=any
 libgles2-mesa-dev deb oldlibs optional arch=any
 libglx-mesa0 deb libs optional arch=any
 mesa-common-dev deb libdevel optional arch=any
 mesa-drm-shim deb libs optional arch=amd64,arm64,armel,armhf,i386,mips64el,powerpc,ppc64,ppc64el,s390x,sparc64
 mesa-libgallium deb libs optional arch=linux-any
 mesa-opencl-icd deb libs optional arch=amd64,arm64,armel,armhf,loong64,mips64el,powerpc,ppc64,ppc64el,riscv64,s390x,x32
 mesa-teflon-delegate deb libs optional arch=arm64
 mesa-va-drivers deb libs optional arch=linux-any profile=!pkg.mesa.nolibva profile:v1=!pkg.mesa.nolibva
 mesa-vdpau-drivers deb libs optional arch=linux-any
 mesa-vulkan-drivers deb libs optional arch=amd64,arm64,armel,armhf,i386,loong64,mips64el,powerpc,ppc64,ppc64el,riscv64,s390x,sparc64,x32
Checksums-Sha1:
 fdc5142bf0deb9888e153bb086122581c4a3bedc 43813260 mesa_25.2.8.orig.tar.xz
 28da6d9fe63e14d316066d1ffc07f613f621fd68 422116 mesa_25.2.8-0ubuntu0.24.04.5.debian.tar.xz
Checksums-Sha256:
 097842f3e49d996868b38688db87b006f7d4541e93ce86d2f341d8b3e7be7c93 43813260 mesa_25.2.8.orig.tar.xz
 edda95cf4f8cf4378a0e6cefc4a62b9db811e2e0d94cb498ea077357dda41ed5 422116 mesa_25.2.8-0ubuntu0.24.04.5.debian.tar.xz
Files:
 c555052c29e6fdfe3cfb68c05707ca09 43813260 mesa_25.2.8.orig.tar.xz
 9cc4b5ade9ccf996271142675af9bc3b 422116 mesa_25.2.8-0ubuntu0.24.04.5.debian.tar.xz
Original-Maintainer: Debian X Strike Force <debian-x@lists.debian.org>

-----BEGIN PGP SIGNATURE-----

iQJHBAEBCgAxFiEECwtuSU6dXvs5GA2aLRkspiR3AnYFAmrfsWoTHHN1cGVybTFA
ZGViaWFuLm9yZwAKCRAtGSymJHcCdt2gEAC929W+Y7+Rt/kdYnTAtL5qU5liPCWR
bcqORf9aT9o3J5s6Gi+4Kubyl2NtM8id8BOjaMHXqV9HucSlbfOx12fZBNN9ppBj
SLNOLhHcC21Al4Mhrbp8kxolaNtvuFclTPt/4z43i9WoDOCXrn4R5a2R/ziPCh1c
pLD5lxZth2M+axpKQEWO9Vsy9Go0dc7OfyEVKQhIvUxj0HTsv2gnWu/8rZSio/jo
HrjFVYhEwkjRR7YTt+8Eu/Hj2TAXRztdUw1t+iHzVBAFhAmZgfdvT5mgD/oAQkDQ
hSLsGCt5Y6tHPGlxOCtPoT1cen2bmvbggxTrdnFYX/5iZvKPL4COAxHN7asOG8rT
OXA4n0REflyGvrVDp1DyRaKzzdQajcFQRIZEKLcn7u/zaxhdZwo2EhfLje4jtIwx
YRooMXNMvb243jTNVwYe1Oh6zt8wU9ZSLOvxseTIoHrnojYZ4ukh+ImzuvZ+XVAW
O9Lit2L3xDGIxQmBDOp7ljCPaEROsbrqiGf3ycyiZDsAhhqC+J4EbCSd7v+8xEYK
nBpuNDTB63b/COQQs6lC1cmRNIeEqKqAGXtlIQHlPpNtPEB4wM26www32TLgers9
PE3vd6xETwwgYA7GE6OD76K7rOxNgjw4oYcIIvsKmRXRf0L5K6404RA7D4shSg/I
2dKyESPl1s1xBw==
=AtLw
-----END PGP SIGNATURE-----
