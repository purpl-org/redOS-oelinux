#!/usr/bin/env bash

# Only to be used by froggitti & ekeleze for redOS auto-updates

# For sending pre-existing builds

clear

read -p "Enter build increment: " inc
export INCREMENT="$inc"

read -p "Enter path to server root key (required, FrogServer): " keypath
export KEY_PATH="$keypath"

eval `ssh-agent`

ssh-add "$KEY_PATH"

echo Touch auto update inhibitor
ssh -p 2222 root@froggitti.net 'touch /all_servers/redos-ota-server/otas/dnar'
sleep 1s

echo Remove old latest file
ssh -p 2222 root@froggitti.net 'rm /all_servers/redos-ota-server/otas/latest'
sleep 1s

echo Make new latest file
ssh -p 2222 root@froggitti.net 'touch /all_servers/redos-ota-server/otas/latest'
sleep 1s

echo Echo new version number to new latest file
ssh -p 2222 root@froggitti.net "echo 0.9.0.$INCREMENT >> /all_servers/redos-ota-server/otas/latest"
sleep 1s

echo Copy Dev OTA
scp -P 2222  _build/vicos-0.9.0."$INCREMENT"d.ota root@froggitti.net:/all_servers/redos-ota-server/otas/full/dev/0.9.0."$INCREMENT".ota

echo Copy OSKR OTA
scp -P 2222 _build/vicos-0.9.0."$INCREMENT"oskr.ota root@froggitti.net:/all_servers/redos-ota-server/otas/full/oskr/0.9.0."$INCREMENT".ota

echo Copy Dev OTA to a never-changing URL
ssh -p 2222 root@froggitti.net "cp /all_servers/redos-ota-server/otas/full/dev/0.9.0."$INCREMENT".ota /all_servers/redos-ota-server/otas/full/latest/dev.ota"
	
echo Copy OSKR OTA to a never-changing URL
ssh -p 2222 root@froggitti.net "cp /all_servers/redos-ota-server/otas/full/oskr/0.9.0."$INCREMENT".ota /all_servers/redos-ota-server/otas/full/latest/oskr.ota"

echo Remove auto update inhibitor
ssh -p 2222 root@froggitti.net 'rm /all_servers/redos-ota-server/otas/dnar'
sleep 1s

ssh -p 2222 root@froggitti.net 'wall redOS build successfully sent to the OTA server.'

echo Done.

