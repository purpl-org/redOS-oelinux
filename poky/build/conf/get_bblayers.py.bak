# generate the bblayers.conf file for the current build workspace
# emitted to stdout for simplicity.

import os, sys, fnmatch, re
from operator import itemgetter

def getLayerPriority(layerConfPath):
    # open the layer.conf and return the bbfile priority or default to 0
    with open(layerConfPath, "r") as confFile:
        for line in confFile:
            fields = line.split()
            if fields and re.match("BBFILE_PRIORITY", fields[0]):
                return int(fields[2].strip("\""))
    return 0  # default priority if not found

def getLayerPaths(target, fnexpr):
    retList = []
    for file in os.listdir(target):
        if (fnmatch.fnmatch(file, fnexpr) and not
            (fnmatch.fnmatch(file, "meta-skeleton") or
             fnmatch.fnmatch(file, "meta-selftest") or
             fnmatch.fnmatch(file, "meta-yocto") or
             fnmatch.fnmatch(file, "meta-yocto-bsp"))):
            layerPath = os.path.join(target, file)
            layerConfPath = os.path.join(layerPath, "conf", "layer.conf")
            if os.path.exists(layerConfPath):
                retList.append((layerPath, getLayerPriority(layerConfPath)))
            if fnmatch.fnmatch(file, "meta-qti-cv-prop"):
                scve = os.path.join(layerPath, "scve")
                fastcv = os.path.join(layerPath, "fastcv")
                retList.append((scve, getLayerPriority(os.path.join(scve, "conf", "layer.conf"))))
                retList.append((fastcv, getLayerPriority(os.path.join(fastcv, "conf", "layer.conf"))))
    return sorted(retList, key=itemgetter(1), reverse=True)

def generatePathString(pathList):
    retString = ""
    for path, _ in pathList:
        retString += path + " "
    return retString.strip()

print("# This configuration file is dynamically generated every time")
print("# set_bb_env.sh is sourced to set up a workspace.  DO NOT EDIT.")
print("#--------------------------------------------------------------")
print("LCONF_VERSION = \"6\"")
print()
print("export WORKSPACE := \"${@os.path.abspath(os.path.join(os.path.dirname(d.getVar('FILE', d)),'../../..'))}\"")
print()
print("BBPATH = \"${TOPDIR}\"")
print("BBFILES ?= \"\"")
print("BBLAYERS = \"" + generatePathString(getLayerPaths(sys.argv[1].strip('\"'), sys.argv[2].strip('\"'))) + "\"")
