SRC_URI += "file://disable-sae.conf"

do_install_append() {
    install -d ${D}${sysconfdir}/wpa_supplicant
    cat ${WORKDIR}/disable-sae.conf \
        >> ${D}${sysconfdir}/wpa_supplicant.conf
}
