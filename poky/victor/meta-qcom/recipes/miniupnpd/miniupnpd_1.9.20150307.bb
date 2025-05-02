inherit autotools-brokensep
SUMMARY = "Lightweight implementation of a UPnP IGD daemon."
DESCRIPTION = "MiniUPnPd is a low memory daemon which acts as a\
UPnP device, enabling seamless detection of other UPnP devices/control points."
HOMEPAGE = "http://miniupnp.free.fr/"
BUGTRACKER = "http://miniupnp.tuxfamily.org/forum/viewforum.php?f=2"
LICENSE = "BSD-3-Clause"
PRIORITY = "optional"
DEPENDS = "conntrack-tools"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/BSD-3-Clause;md5=550794465ba0ec5312d6919e203a55f9"

# Package Revision (update whenever recipe is changed)
PR = "r7"

SRC_URI = "\
    https://www.codeaurora.org/mirrored_source/quic/le/${PN}-${PV}.tar.gz \
    file://0001-certification-fixes.patch \
    file://0001-presentation-page.patch \
    file://0001-port-desc.patch \
    file://0001-security-fix.patch \
"

SRC_URI[md5sum] = "f91dc5647b1d2c13a82082a481a53e3d"
SRC_URI[sha256sum] = "7d8c9b1f7ed73e288b4e7e52e0af67de73ba07994a6984008a1a688568153409"

do_configure[noexec] = "1"

do_compile () {
    cd ${S} && make -f Makefile.linux LIBDIR=${STAGING_LIBDIR} INCDIR=${STAGING_INCDIR} STRIP=echo
}

do_install () {
    make -f Makefile.linux DESTDIR=${D} LIBDIR=${STAGING_LIBDIR} STRIP=echo install
}
