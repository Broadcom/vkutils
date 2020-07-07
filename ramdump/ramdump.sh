#!/bin/bash

# The script will dump contents of RAM to a file
# Which can be used for crash dump analysis
# Timestamp and device_id are appended to the end of the filename

# Command to reset the card from ramdump mode
# vkcli <dev_num> reset

# Code: 0xb0000000 => Remap the PCIe BAR2 to the next 64MB of DDR
# Code: 0xc0000000 => Remap the PCIe BAR2 to the default map base (0x60000000)

if [[ $1 == "--help" || $# -gt 2 ]]
then
        echo "Usage: ./${0} [dev_num] [full]"
        echo -e ' \n\t '"dev_num: 0..11 - corresponds to /dev/bcm-vk.X; w/o dev_num = 0"
        echo -e ' \t '"full: to capture full 2GB RAM image; w/o limit to first 64MB"
        echo -e ' \n '
        exit 0
fi

VKCLI=vkcli

dev_id=0
if [[ $# -ge 1 ]]
then
        dev_id=$1
fi

ramdump_mode=$(cat "/sys/class/misc/bcm-vk.${dev_id}/pci/vk-card-mon/alert_ramdump")
if [[ "$ramdump_mode" -eq "0" ]]
then
        echo "selected device ${dev_id} is not in RAMDUMP mode"
        echo "RAMDUMP EXIT"
        exit 0
fi


echo "Dumping DTCM.."
$VKCLI $dev_id rf 1 0x0 0x40000 dtcm_dump_$(date +%Y%m%d-%H%M%S)_BCM_$dev_id.bin
echo "Dumping ITCM.."
$VKCLI $dev_id rf 1 0x100000 0x40000 itcm_dump_$(date +%Y%m%d-%H%M%S)_BCM_$dev_id.bin
echo "Dumping SCR SRAM.."
$VKCLI $dev_id rf 1 0x200000 0x40000 scr_sram_dump_$(date +%Y%m%d-%H%M%S)_BCM_$dev_id.bin
echo "Dumping A72 console log.."
$VKCLI $dev_id rf 1 0x600000 0x100000 a72_console_dump_$(date +%Y%m%d-%H%M%S)_BCM_$dev_id.log

echo "Dumping DDR.."
# set the pcie window to default map base 0x60000000
$VKCLI $dev_id wb 0 0x41c 0xc0000000

# Dumps 2GB of DDR by remapping the pcie bar2 window
# 2GB/64MB = 32
max_slots=1
if [[ $2 == "full" ]]
then
        max_slots=32
fi

for (( i=1; i <= $max_slots; i++ ))
do {
  echo "---------------------------------"
  rm -rf ddr_dump_t.bin
  $VKCLI $dev_id rf 2 0x0 0x4000000 ddr_dump_t.bin
  # Request card to remap the pcie BAR2 to the next 64MB
  $VKCLI $dev_id wb 0 0x41c 0xb0000000
  cat ddr_dump_t.bin >> ddr_dump.bin
  echo $i
  echo "---------------------------------"
} done
mv ddr_dump.bin ddr_dump_$(date +%Y%m%d-%H%M%S)_BCM_$dev_id.bin
rm -rf ddr_dump_t.bin
sync
echo "RAMDUMP DONE"
echo "To reset the card execute: ${VKCLI} <dev_num> reset"
