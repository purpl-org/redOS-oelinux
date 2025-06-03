inherit qcommon qlicense qprebuilt

DESCRIPTION = "Qualcomm MSM Interface (QMI) Library"

PR = "r13"
SRC_DIR = "${WORKSPACE}/qmi"
S = "${WORKDIR}/qmi"

DEPENDS = "configdb diag dsutils libcutils"
RDEPENDS:${PN} = "dsutils libcutils"

CFLAGS += "-I${STAGING_INCDIR}/cutils"
CFLAGS += " -fforward-propagate"
CFLAGS:append_mdm9650 += "-DFEATURE_QMUXD_DISABLED"

EXTRA_OECONF = "--with-qxdm \
                --with-common-includes=${STAGING_INCDIR}"

EXTRA_OECONF:append_msm8960 = " --enable-auto-answer=yes"
