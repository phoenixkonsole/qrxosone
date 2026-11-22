-----BEGIN PGP SIGNED MESSAGE-----
Hash: SHA512

Format: 3.0 (quilt)
Source: busybox
Binary: busybox, busybox-static, busybox-initramfs, busybox-udeb, busybox-syslogd, udhcpc, udhcpd
Architecture: any all
Version: 1:1.36.1-6ubuntu3.1
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
 1079338b95cc8ec557dde56d19b5837f771dfe40 84488 busybox_1.36.1-6ubuntu3.1.debian.tar.xz
Checksums-Sha256:
 b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314 2525473 busybox_1.36.1.orig.tar.bz2
 d4a7082ba3cbc35b2fdb4f132a32e3789de66ce468b7b4858563f5f157c28afe 195 busybox_1.36.1.orig.tar.bz2.asc
 1c7d785cf1e1d5d09ddc22fe755e14327fb3799878a5d840fc611044ff05f022 84488 busybox_1.36.1-6ubuntu3.1.debian.tar.xz
Files:
 0fc591bc9f4e365dfd9ade0014f32561 2525473 busybox_1.36.1.orig.tar.bz2
 4b358c67a2c99a1e86c8c52ab374b89f 195 busybox_1.36.1.orig.tar.bz2.asc
 060a7c580d5a4a6764cd60c0e8c254d0 84488 busybox_1.36.1-6ubuntu3.1.debian.tar.xz
Original-Maintainer: Debian Install System Team <debian-boot@lists.debian.org>

-----BEGIN PGP SIGNATURE-----

iQIzBAEBCgAdFiEER/Iq56J1QpFyK/CQ9uFA9ts1nlgFAma7foUACgkQ9uFA9ts1
nlikeQ//bjsdC5L7mtv2Nrb1lWkYlhsBixCi8ByCqu+opjZkEU0tbRT2fi7l1DY5
jRe6kW1/ZcFLuuE9NiMc6rLN9R6LIgmWZzp9a2nVSUDxmEq9gvec4gdWQVNeosiF
PGZoidqCBQUtQ0rc6MNcB/jwuiWKgWmojsqFLDXerHvidwwfPNeCgJJhrG+EcWbD
bdt9HxzlpVyQUeY7smbXo6JFvz8eELFACGPg1QFZDWY/0F3kRr5DxPQoUXXyjoqS
GzTe+eE0f2KNSwCsL7jiD66uoZYi12jEYouq3fdnHfvqg82a9NLbxkCsr67Slde1
LYnf9A7JmEaqeDbBxU3mEZ6iIcmJRsekaaVPlCCGEvApbFFl81WJFQSl9ib/61r9
ZOVV82lbSS/Rw1Rp+pyzR5RPyVxJxayVfel5VyqXsAnIBUapIJVnEeFNn6Se+433
kYgnikZ7vj1jqvZ08FHhQO+pnXo2x3yxeBA7L0rULSqwPBg01cuPZxiZQzrVbJZU
N0oUREbstDEcnJCuCT0w2ISyPYpsm6Bkv+hmLD/xrvZ8dsJraHp03DRuKS5/IC1Q
DKCK8NO52Y3jZ99+0XWyZMeP0kUUpOeQ00OXxJj45j1cEZy1HNxJxtGC7Z0NZv61
r9cZgoqdf6XaHfv3Ga7x8ze0KrT7WzbyCi5pqrZH7fuMo6qtVK4=
=P6b2
-----END PGP SIGNATURE-----
