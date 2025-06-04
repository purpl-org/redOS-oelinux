CC = "${WORKSPACE}/old-toolchain/arm/bin/arm-linux-gnueabi-gcc"
CXX = "${WORKSPACE}/old-toolchain/arm/bin/arm-linux-gnueabi-g++"
LD = "${WORKSPACE}/old-toolchain/arm/bin/arm-linux-gnueabi-ld"

REMOVE_MAPS = "-fcanon-prefix-map(=[^ ]*)?|-fmacro-prefix-map(=[^ ]*)?|-fdebug-prefix-map(=[^ ]*)?|-ffile-prefix-map(=[^ ]*)?"

CFLAGS:remove_pats = "${REMOVE_MAPS}"
CXXFLAGS:remove_pats = "${REMOVE_MAPS}"
LDFLAGS:remove_pats = "${REMOVE_MAPS}"

# python do_prefix_cleanup() {
#     import re
#     patterns = [
#         r'-fcanon-prefix-map=[^ ]*',
#         r'-fcanon-prefix-map',
#         r'-fmacro-prefix-map=[^ ]*',
#         r'-fdebug-prefix-map=[^ ]*',
#         r'-ffile-prefix-map=[^ ]*',
#     ]
#     for var in ['CFLAGS', 'CXXFLAGS', 'LDFLAGS']:
#         val = d.getVar(var, True) or ''
#         cleaned = val
#         for p in patterns:
#             cleaned = re.sub(p, '', cleaned)
#         cleaned = re.sub(r'\s+', ' ', cleaned).strip()
#         cleaned = "isthisworkging"
#         d.setVar(var, cleaned)
# }

# addtask prefix_cleanup before do_configure

INSANE_SKIP:${PN} += "32bit-time"
