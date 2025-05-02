DEPENDS += "virtual/kernel"

FILESEXTRAPATHS:prepend_mdm := "${THISDIR}/files:"
SRC_URI:append_mdm = " \
        file://103-ubicom32-nattype_lib.patch \
"

CFLAGS:append_mdm = "-I${STAGING_KERNEL_BUILDDIR}/usr/include/linux/netfilter_ipv4"
