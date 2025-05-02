LICENSE = "MIT"

EXTRA_OECONF += " --localstatedir=/data"

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI += "file://connman.conf \
			file://connman.service \
			file://main.conf"

do_install:append() {
	install -d ${D}/etc/dbus-1/system.d
	install -m 0644 ${UNPACKDIR}/connman.conf ${D}/etc/dbus-1/system.d/connman.conf
	
	rm -rf ${D}/usr/lib/systemd/system/connman.service ${D}/etc/systemd/system/connman.service ${D}/usr/lib/systemd/system/multi-user.target.wants ${D}/etc/systemd/system/multi-user.target.wants

	install -d ${D}/usr/lib/systemd/system
	install -d ${D}/usr/lib/systemd/system/multi-user.target.requires

	install -m 0644 ${UNPACKDIR}/connman.service ${D}/usr/lib/systemd/system/connman.service
	ln -sf /usr/lib/systemd/system/connman.service ${D}/usr/lib/systemd/system/multi-user.target.requires

	install -d ${D}/etc/connman
	install -m 0644 ${UNPACKDIR}/main.conf ${D}/etc/connman/main.conf
}

FILES:${PN} += "/etc/dbus-1/system.d \
				/usr/lib/systemd/system \
				/usr/lib/systemd/system/multi-user.target.requires \
				/etc/connman/main.conf"