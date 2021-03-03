#!/bin/bash

# The script will collect:
#   - content of sysfs entries
#   - vksim_logdump
#   - memstats
#   - memstats all
#   - healthstats
#   - vksim_ctx_dump
#   - ramdump (first 64MB) unless "--noram" option present in cmd line


if [[ $1 == "--help" || $# -gt 2 ]]
then
        echo "Usage: ./${0} [dev_num] [--noram]"
        echo -e ' \n\t '"dev_num: 0..11 - corresponds to /dev/bcm-vk.X; w/o dev_num = 0"
        echo -e ' \n\t '"--noram: - skip RAMDUMP capture"
        echo -e ' \n '
        exit 0
fi

#check the driver is loaded
lsmod | grep bcm_vk >& /dev/null

if [ $? -eq 0 ]
then
    echo "bcm_vk.ko has been loaded"
else
    echo "bcm_vk.ko has NOT been loaded - not found"
    exit 1
fi

#detect the OS - necessary due to xargs parameters handling
is_yocto=0
uname -a | grep genericx86-64 >& /dev/null
if [[ $? -eq 0 ]]
then
    is_yocto=1
fi

VKCMD=vkcmd

DEVICE_ID=0
if [[ $# -ge 1 ]] && [[ $1 != "--noram" ]]
then
        DEVICE_ID=$1
fi

get_ramdump=1
if [[ $1 == "--noram" ]] || [[ $2 == "--noram" ]]
then
        get_ramdump=0
fi

c_ts=$(date +%Y%m%d-%H%M%S)
c_fn=vk_devinfo_${c_ts}_BCM_${DEVICE_ID}.txt
pci_path=/sys/class/misc/bcm-vk.${DEVICE_ID}/pci
d_id=/dev/bcm-vk.$DEVICE_ID
card_in_ramdump="$(cat ${pci_path}/vk-card-mon/alert_ramdump)"
out=sys_$DEVICE_IDi.txt
subst='FNR==1{print FILENAME; }{ print }'

touch $c_fn
echo " Valkyrie dump sysfs ..." >> $out
if [[ is_yocto -eq 1 ]]
then
        find $pci_path/vk-card-status/ -type f -print | sort -z | xargs -r awk "${subst}" > $out
        find $pci_path/vk-card-mon/ -type f -print | sort -z | xargs -r awk "${subst}" >> $out
else
        find $pci_path/vk-card-status/ -type f -print0 | sort -z | xargs -r0 awk "${subst}" > $out
        find $pci_path/vk-card-mon/ -type f -print0 | sort -z | xargs -r0 awk "${subst}" >> $out
fi

echo "########################" >> $out

if [[ card_in_ramdump == "0" ]]
then
        echo " Valkyrie dump log ..." >> $out
        echo "########################" >> $out
        $VKCMD -d $d_id -c vksim_logdump -s 5 >> $out
        echo "########################" >> $out
        $VKCMD -d $d_id -c memstats >> $out
        echo "########################" >> $out
        $VKCMD -d $d_id -c memstats all >> $out
        echo "########################" >> $out
        $VKCMD -d $d_id -c healthstats >> $out
        echo "########################" >> $out
        $VKCMD -d $d_id -c vksim_ctx_dump >> $out
else
        echo " Device ${DEVICE_ID} is in ramdump mode; skip logs based on vkcmd"
        if [[ $get_ramdump == 1 ]]
        then
                #short ramdump (i.e. first 64MB)
                source ramdump.sh $DEVICE_ID
        fi
fi

mv $out $c_fn


sync
echo "${0} DONE"
