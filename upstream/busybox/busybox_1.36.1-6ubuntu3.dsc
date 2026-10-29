-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA512

Format: 3.0 (quilt)
Source: busybox
Binary: busybox, busybox-static, busybox-initramfs, busybox-udeb, busybox-syslogd, udhcpc, udhcpd
Architecture: any all
Version: 1:1.36.1-6ubuntu3
Maintainer: Ubuntu Developers <ubuntu-devel-discuss@lists.ubuntu.com>
Uploaders:  Chris Boot <bootc@debian.org>, Christoph Biedl <debian.axhn@manchmal.in-ulm.de>, Michael Tokarev <mjt@tls.msk.ru>,
Homepage: http://www.busybox.net
Standards-Version: 4.6.2
Vcs-Browser: https://salsa.debian.org/installer-team/busybox
Vcs-Git: https://salsa.debian.org/installer-team/busybox.git
Build-Depends: debhelper-compat (= 13), zip <!nocheck>
Package-List:
 busybox deb utils optional arch=any
 busybox-initramfs deb shells optional arch=any
 busybox-static deb shells optional arch=any
 busybox-syslogd deb utils optional arch=all
 busybox-udeb udeb debian-installer optional arch=any
 udhcpc deb net optional arch=all
 udhcpd deb net optional arch=all
Checksums-Sha1:
 a5d40ca0201b20909f7a8a561adf57adccc8a877 2525473 busybox_1.36.1.orig.tar.bz2
 6b10ee303531a1df9b18f7ddf054c341b6a7d9e9 195 busybox_1.36.1.orig.tar.bz2.asc
 b2bb793aeb6095f1499d82f358bdd771133f2490 79372 busybox_1.36.1-6ubuntu3.debian.tar.xz
Checksums-Sha256:
 b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314 2525473 busybox_1.36.1.orig.tar.bz2
 d4a7082ba3cbc35b2fdb4f132a32e3789de66ce468b7b4858563f5f157c28afe 195 busybox_1.36.1.orig.tar.bz2.asc
 8a4f6e33fdb9da0180d9c6070288d4c701753e25e0d3cae12acdb8c926491ff1 79372 busybox_1.36.1-6ubuntu3.debian.tar.xz
Files:
 0fc591bc9f4e365dfd9ade0014f32561 2525473 busybox_1.36.1.orig.tar.bz2
 4b358c67a2c99a1e86c8c52ab374b89f 195 busybox_1.36.1.orig.tar.bz2.asc
 4923c05110298cd2d7459975d722e670 79372 busybox_1.36.1-6ubuntu3.debian.tar.xz
Original-Maintainer: Debian Install System Team <debian-boot@lists.debian.org>

-----BEGIN PGP SIGNATURE-----

iQIzBAEBCgAdFiEEpi0s+9ULm1vzYNVLFZ61xO/Id0wFAmYiKFIACgkQFZ61xO/I
d0xDMg//UxHDFWTp5pplRG4WaHvAJXSpnF4bkZIvb694QDzywNgfHTygz8xo++pX
8zUcyO0vBuZcxJgS2aakB251a/2hZivMj82xD6MEAUQeDld4nN+ELqG3JeEd5wBe
r/iLfKmARJlh1KESDWQyVWpHSnF4JWgMmVqxL5cEukCT0+5MA5PgQ5y6uM8Hi5lH
Q7v3hQECrSsNDJ9/cHIUWGAeyf1sJfCFayNKvoGXOrp/5FhGQYfMF8QV/WF9I9CZ
Bm294O229KJRVRbinLg8IAwEtRTyZG8neiV2UInI+SA+1rSnTN8oL671/xtKMRRU
iUzsbiTl9R2na5x6xQfzFQQYbjcWn7TCNrgInh3Y5uskabF7fMBhr02qpT8pCiD4
N4K5U02/RT4mmvA6BlCBi3/mDR3eaNS4YD5gQVJ4Nv27Su56mvebxy0vWP8bRnpm
x35guyooTXE9mDPREz3FWmS9u4CUFXqqZ2TyTHB8oehtUpnJuSQOrzQC+61B+BZf
eZFX8RhJKj6fBTXMldmGKIulRBo3e9I1DMoPrqS+GvaeVCh4JaNJWQGJ1sKcRr6N
hsnRlUoCLGvxeTmho1YPyh5vD2bAIVyLi6W55KNpQqnxynbHkacBZ9LJweUJMM+V
dZElcNsLxpFDwlVOCGtVBakxVgkYdXpxDTiw7AHAayVn8pL5WQI=
=cW5N
-----END PGP SIGNATURE-----
