# WLAN open source Packages
require apq8017-wlan-image.inc
IMAGE_INSTALL += "qcacld-hl"
IMAGE_INSTALL:remove = "wcnss"
IMAGE_INSTALL:remove = "wlan-opensource"
IMAGE_INSTALL += "qcacld-utils"
IMAGE_INSTALL += "wlan-services"
