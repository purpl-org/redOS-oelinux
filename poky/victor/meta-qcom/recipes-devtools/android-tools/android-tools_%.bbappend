do_install:append() {
    mv ${D}${bindir}/make_ext4fs ${D}${bindir}/make_ext4fs-android
}
