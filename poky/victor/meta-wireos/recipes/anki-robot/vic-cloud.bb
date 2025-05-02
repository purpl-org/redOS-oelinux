DESCRIPTION = "Victor Cloud Services daemon"
LICENSE = "Anki-Inc.-Proprietary"                                                                   
LIC_FILES_CHKSUM = "file://${COREBASE}/../victor/meta-qcom/files/anki-licenses/\                           
Anki-Inc.-Proprietary;md5=4b03b8ffef1b70b13d869dbce43e8f09"

SERVICE_FILE = "vic-cloud.service"
# GOINSTALLER="go1.15.6.linux-amd64.tar.gz"

SRC_URI = "file://${SERVICE_FILE}"
S = "${WORKDIR}/sources"
UNPACKDIR = "${S}"

inherit systemd

DEPENDS = "pkgconfig-native"

do_install:append () {
   if ${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'true', 'false', d)}; then
       install -d ${D}${systemd_unitdir}/system/
       install -m 0644 ${S}/${SERVICE_FILE} -D ${D}${systemd_unitdir}/system/${SERVICE_FILE}
   fi
}

FILES:${PN} += "${systemd_unitdir}/system/"
SYSTEMD_SERVICE:${PN} = "${SERVICE_FILE}"

inherit externalsrc

EXTERNALSRC = "${WORKSPACE}/anki/vector-cloud"

GID_ANKI      = '2901'
GID_CLOUD     = '888'
GID_ANKINET   = '2905'

UID_NET       = "${GID_ANKINET}"
UID_CLOUD     = "${GID_CLOUD}"

do_compile() {
    # mkdir -p "${GOPATH}"
    # mkdir -p "${GOEXEPATH}"

    # if [ ! -f "${GOEXEPATH}/bin/go" ]; then
    #    wget -P "${WORKDIR}" "https://golang.org/dl/${GOINSTALLER}"
    #    tar zxvf "${WORKDIR}/${GOINSTALLER}" -C "${GOEXEPATH}"
    # fi

    cd "${EXTERNALSRC}"
    # export GOPATH="${GOPATH}"
    # export PATH="${GOEXEPATH}/go/bin:${PATH}"
    # using system Go
    make all
}

do_install () {
    mkdir -p ${D}/anki/bin
    mkdir -p ${D}/anki/lib
    cp ${WORKSPACE}/anki/vector-cloud/build/* ${D}/anki/bin
    cp ${WORKSPACE}/anki/vector-cloud/armlibs/lib/libopus.so.0.7.0 ${D}/anki/lib/libopus.so.0
}

do_package_qa[noexec] = "1"

INSANE_SKIP:${PN} = " already-stripped ldflags dev-elf"
EXCLUDE_FROM_SHLIBS = "1"

FILES:${PN} += "anki/bin/vic-cloud"
FILES:${PN} += "anki/bin/vic-gateway"
FILES:${PN} += "anki/lib/libopus.so.0"
