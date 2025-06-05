SUMMARY = "Extra OS configuration"
DESCRIPTION = "configuring qualcomm things"
SECTION = "examples"
PR = "r1"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/${LICENSE};md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://initscripts \
	   file://services \
	   file://other"

#S = "${WORKDIR}"
S = "${WORKDIR}/sources"
UNPACKDIR = "${S}"

do_install () {
	install -d ${D}/etc/initscripts
	install -d ${D}/usr/lib/systemd/system/multi-user.target.wants
	install -d ${D}/usr/lib/systemd/system/local-fs.target.requires
	install -d ${D}/usr/sbin
	cp -r ${S}/other/export-gpio ${D}/usr/sbin/export-gpio
	cp -r ${S}/other/set-timezone ${D}/usr/sbin/set-timezone
	cp -r ${S}/initscripts/* ${D}/etc/initscripts/
	chmod 0777 ${D}/etc/initscripts/*
	cp -r ${S}/services/* ${D}/usr/lib/systemd/system/
	ln -sf /usr/lib/systemd/system/anki-audio-init.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	# this is now installed by the update-engine recipe
	#ln -sf /usr/lib/systemd/system/boot-successful.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	ln -sf /usr/lib/systemd/system/logd.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	#ln -sf /usr/lib/systemd/system/mdsprpcd.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	ln -sf /usr/lib/systemd/system/mm-anki-camera.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	ln -sf /usr/lib/systemd/system/mm-qcamera-daemon.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	#ln -sf /usr/lib/systemd/system/qtid.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	#ln -sf /usr/lib/systemd/system/qti_system_daemon.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	#ln -sf /usr/lib/systemd/system/rmt_storage.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	#ln -sf /usr/lib/systemd/system/init_audio.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	#ln -sf /usr/lib/systemd/system/ankibluetoothd.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	#ln -sf /usr/lib/systemd/system/btproperty.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	#ln -sf /usr/lib/systemd/system/leprop.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	ln -sf /usr/lib/systemd/system/mount-data.service ${D}/usr/lib/systemd/system/local-fs.target.requires/
	#ln -sf /usr/lib/systemd/system/setup-qtiroot.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
	#ln -sf /usr/lib/systemd/system/setup-persist.service ${D}/usr/lib/systemd/system/multi-user.target.wants/
}

FILES:${PN} = "	/usr/lib/systemd/system \
		/usr/lib/systemd/system/multi-user.target.wants \
		/etc/initscripts \
		/usr/sbin/export-gpio \
		/usr/sbin/set-timezone"

INSANE_SKIP:${PN} = "file-rdeps"

# Prevents do_package failures with:
# debugsources.list: No such file or directory:
INHIBIT_PACKAGE_DEBUG_SPLIT = "1"
