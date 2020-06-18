#!/bin/bash

# The script will dump contents of RAM to a file
# Which can be used for crash dump analysis
# Timestamp is appended to the end of the filename

# Command to reset the card from ramdump mode
# vkcli /dev/bcm-vk.<dev_num> wb 0 0x41c 0xa0000000

# Code: 0xa0000000 => Code to reset the card from ramdump mode
# Code: 0xb0000000 => Remap the PCIe BAR2 to the next 64MB of DDR
# Code: 0xc0000000 => Remap the PCIe BAR2 to the default map base (0x60000000)

if [ $# -ne 1 ]
then
        echo "Usage: ./ramdump.sh <dev_num>"
        exit 0
fi

echo "Dumping DTCM.."
vkcli /dev/bcm-vk.$1 rf 1 0x0 0x40000 dtcm_dump_$(date +%Y%m%d-%H%M%S).bin
echo "Dumping ITCM.."
vkcli /dev/bcm-vk.$1 rf 1 0x100000 0x40000 itcm_dump_$(date +%Y%m%d-%H%M%S).bin
echo "Dumping SCR SRAM.."
vkcli /dev/bcm-vk.$1 rf 1 0x200000 0x40000 scr_sram_dump_$(date +%Y%m%d-%H%M%S).bin

echo "Dumping DDR.."
# set the pcie window to default map base 0x60000000
vkcli /dev/bcm-vk.$1 wb 0 0x41c 0xc0000000

# Dumps 2GB of DDR by remapping the pcie bar2 window
# 2GB/64MB = 32
for (( i=1; i <= 32; i++ ))
do {
  echo "---------------------------------"
  rm -rf ddr_dump_t.bin
  vkcli /dev/bcm-vk.$1 rf 2 0x0 0x4000000 ddr_dump_t.bin
  # Request card to remap the pcie BAR2 to the next 64MB
  vkcli /dev/bcm-vk.$1 wb 0 0x41c 0xb0000000
  cat ddr_dump_t.bin >> ddr_dump.bin
  echo $i
  echo "---------------------------------"
} done
mv ddr_dump.bin ddr_dump_$(date +%Y%m%d-%H%M%S).bin
rm -rf ddr_dump_t.bin
sync
echo "RAMDUMP DONE"
echo "To reset the card execute: vkcli /dev/bcm-vk.<dev_num> reset"
