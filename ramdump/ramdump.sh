#!/bin/bash

# The script will dump contents of RAM to a file
# Which can be used for crash dump analysis
# Timestamp and device_id are appended to the end of the filename

# Command to reset the card from ramdump mode
# vkcli <dev_num> reset

# Code: 0xb0000000 => Remap the PCIe BAR2 to the next 64MB of DDR
# Code: 0xc0000000 => Remap the PCIe BAR2 to the default map base (0x60000000)

while getopts ":h" opt; do
  case ${opt} in
    h )
      echo "Usage:"
      echo "    ramdump -h                      Display this help message."
      echo \
	 "    ramdump <dev_num> [full] [ddr_dump_addr <addr> size <size>] [dtcm] [itcm] [sram] [a72_log]"
      /
      echo -e ' \n\t '"dev_num: 0..11 - corresponds to /dev/bcm-vk.X; w/o dev_num = 0"
      echo -e ' \t '"full: to capture full 2GB RAM image; w/o limit to first 64MB"
      echo -e ' \t '"dtcm: to capture only dtcm region"
      echo -e ' \t '"itcm: to capture only itcm region"
      echo -e ' \t '"sram: to capture only sram region"
      echo -e ' \t '"ddr_dump_addr <addr> size <size>: to capture only specific ddr region"
      echo -e ' \t '"a72_log: to capture only a72 log"
      echo -e ' \t '"ddr_mve_queue: to capture only ddr_mve_queue dump"
      exit 0
      ;;
   \? )
     echo "Invalid Option: -$OPTARG" 1>&2
     exit 1
     ;;
  esac
done
shift $((OPTIND -1))

VKCLI=vkcli

dev_id=0
if [[ $# -ge 1 ]]
then
        dev_id=$1
fi

if ! [[ "$dev_id" =~ ^[0-9]+$ ]]
    then
        echo "dev_num should be integer"
	exit l;
fi

ramdump_mode=$(cat "/sys/class/misc/bcm-vk.${dev_id}/pci/vk-card-mon/alert_ramdump")
if [[ "$ramdump_mode" -eq "0" ]]
then
        echo "selected device ${dev_id} is not in RAMDUMP mode"
        echo "RAMDUMP EXIT"
        exit 0
fi

ddr_dump_addr=0
ddr_dump_size=0
ddr_full=0
dump_all=1
dtcm=0
itcm=0
sram=0
a72_log=0
ddr_mve_queue=0
if (( $# > 1 )); then
	shift
	while [[ $# -gt 0 ]]; do
		case "$1" in
		ddr_dump_addr)
			ddr_dump_addr=$(($2))
			shift
			shift
			case "$1" in
			size)
				ddr_dump_size=$(($2))
				dump_all=0
				shift
			;;
			*)
				echo "Invalid argument: $1"
				exit 1
			esac
			;;
		full)
			dump_all=1
			ddr_full=1
			;;
		dtcm)
			dump_all=0
			dtcm=1
			;;
		itcm)
			dump_all=0
			itcm=1
			;;
		sram)
			dump_all=0
			sram=1
			;;
		a72_log)
			dump_all=0
			a72_log=1
			;;
		ddr_mve_queue)
			dump_all=0
			ddr_mve_queue=1
			;;
		*)
			echo "Invalid argument: $1"
			exit 1
		esac
		shift
	done
fi

echo collecting ramdump for device bcm_vk.$dev_id

FE="$(date +%Y%m%d-%H%M%S)_BCM_$dev_id"
BAR1_DTCM_OFFSET=0x0
BAR1_ITCM_OFFSET=0x100000
BAR1_SRAM_OFFSET=0x200000
A72_CONSOLE_LOG_OFFSET=0x600000
CMD_REMAP_TO_DEFAULT=0xc0000000
CMD_REMAP_NXT_64MB=0xb0000000
FW_STATUS_ADDR_OFFSET=0x41c
DUMP_SIZE_256KB=0x40000
DUMP_SIZE_64MB=0x4000000
DUMP_SIZE_2GB=0x80000000
A72_CONSOLE_LOG_SIZE=0x100000
DDR_START_ADDR=0x60000000
DDR_MVE_QUEUE_START_ADDR=0x70000000
DDR_MVE_QUEUE_SIZE=0x8000000
PCIE_BAR0=0
PCIE_BAR1=1
PCIE_BAR2=2
TIMEOUT_COUNT=10
ddr_dump_from=$DDR_START_ADDR

check_status()
{
	if [[ $? != 0 ]]; then
		echo "ERROR $1"
		exit;
	fi
}

function wait_cmd_to_fnish()
{
        cmd_status=`eval $1 | sed -n '/^Read Bar$/{n;p}'`
        ELAPSED=0
        while [ $(( cmd_status & 0xf0000000)) -ne 0 ]
        do
                if [[ $ELAPSED -eq $TIMEOUT_COUNT ]]; then
                        echo 1;
                        return;
                fi
                (( ELAPSED++ ))
                sleep 0.01 #1msec delay
                cmd_status=`eval $1 | sed -n '/^Read Bar$/{n;p}'`
        done
        echo 0
        return
}


if (( $dump_all)) || (($dtcm)); then
	echo "Dumping DTCM from offset $BAR1_DTCM_OFFSET and size $DUMP_SIZE_256KB"
	$VKCLI $dev_id rf $PCIE_BAR1 $BAR1_DTCM_OFFSET $DUMP_SIZE_256KB dtcm_dump_${FE}.bin
	check_status "DTCM DUMP"
	sync;
fi

if (( $dump_all)) || (($itcm)); then
	echo "Dumping ITCM from offset $BAR1_ITCM_OFFSET and size $DUMP_SIZE_256KB"
	$VKCLI $dev_id rf  $PCIE_BAR1 $BAR1_ITCM_OFFSET $DUMP_SIZE_256KB itcm_dump_${FE}.bin
	check_status "ITCM DUMP"
	sync;
fi

if (( $dump_all)) || (($sram)); then
	echo "Dumping SCR SRAM from offset $BAR1_SRAM_OFFSET and size $DUMP_SIZE_256KB"
	$VKCLI $dev_id rf $PCIE_BAR1 $BAR1_SRAM_OFFSET $DUMP_SIZE_256KB scr_sram_dump_${FE}.bin
	check_status "SRAM DUMP"
	sync;
fi

if (( $dump_all)) || (($a72_log)); then
	echo "Dumping A72 console log from offset $A72_CONSOLE_LOG_OFFSET and size $A72_CONSOLE_LOG_SIZE"
	$VKCLI $dev_id rf ${PCIE_BAR1} $A72_CONSOLE_LOG_OFFSET $A72_CONSOLE_LOG_SIZE a72_console_dump_${FE}.log
	check_status "A72 console log DUMP"
	sync;
fi

#Dump DDR
if (( $dump_all)) || (($ddr_dump_addr)); then
	echo "Dumping DDR.."
	# set the pcie window to default map base 0x60000000
	$VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_TO_DEFAULT
	check_status "REMAP"
	result=$(wait_cmd_to_fnish "$VKCLI $dev_id rb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET")
	if [[ $result -ne 0 ]]; then
	        echo "Failed to write cmd sudo $VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_TO_DEFAULT"
	        exit 1;
	fi
fi

if (( $dump_all)); then
	max_slots=1
	i=1;
	if (( $ddr_full )); then
		# Dumps 2GB of DDR by remapping the pcie bar2 window
		# 2GB/64MB = 32
		max_slots=$(($DUMP_SIZE_2GB/$DUMP_SIZE_64MB))
	fi
	while true;
	do {
		echo "---------------------------------"
		rm -rf ddr_dump_t.bin
		echo "DDR dump from 0x$( printf "%x" $ddr_dump_from) to 0x$( printf "%x" $(($ddr_dump_from+$DUMP_SIZE_64MB-1)))"
		$VKCLI $dev_id rf $PCIE_BAR2 0x0 $DUMP_SIZE_64MB ddr_dump_t.bin
		check_status "DDR DUMP"
		sync;
		cat ddr_dump_t.bin >> ddr_dump_${FE}.bin
		ddr_dump_from=$((ddr_dump_from+$DUMP_SIZE_64MB))
		i=$((i+1));
		if (( $i > $max_slots )); then
			echo "---------------------------------"
			break;
		fi
		# Request card to remap the pcie BAR2 to the next 64MB
		echo "Remap the pcie BAR2 to the next 64MB to read from offset 0x$( printf "%x" $ddr_dump_from)"
		$VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_NXT_64MB
		check_status "REMAP"
		#Check command status
		result=$(wait_cmd_to_fnish "$VKCLI $dev_id rb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET")
		if [[ $result -ne 0 ]]; then
		        echo "Failed to write cmd sudo $VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_NXT_64MB"
		        exit 1;
		fi
	} done
fi


function set_pci_window_to_ddr_base {
	# set the pcie window to default map base 0x60000000
	$VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_TO_DEFAULT
	check_status "REMAP"
	result=$(wait_cmd_to_fnish "$VKCLI $dev_id rb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET")
	if [[ $result -ne 0 ]]; then
	        echo "Failed to write cmd sudo $VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_TO_DEFAULT"
	        exit 1;
	fi
}

function dump_ddr_range {
	ddr_dump_addr_tmp=$1
	ddr_dump_size_tmp=$2
	file_name=$3
	ddr_dump_from=$DDR_START_ADDR
	set_pci_window_to_ddr_base

	LOOP_END_INDEX=$((($ddr_dump_addr_tmp - $DDR_START_ADDR)/$DUMP_SIZE_64MB))
	window_start_offset=$((($ddr_dump_addr_tmp - $DDR_START_ADDR)%$DUMP_SIZE_64MB))
	window_size=$(($DUMP_SIZE_64MB-$window_start_offset))

	# Move the window to the starting offset
	for (( i=1; i <= $LOOP_END_INDEX; i++ ))
	do {
		# Request card to remap the pcie BAR2 to the next 64MB
		ddr_dump_from=$((ddr_dump_from+$DUMP_SIZE_64MB))
		echo "Remap the pcie BAR2 to the next 64MB to offset 0x$( printf "%x" $ddr_dump_from)"
		$VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_NXT_64MB
		check_status "REMAP"
		#Check command status
		result=$(wait_cmd_to_fnish "$VKCLI $dev_id rb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET")
		if [[ $result -ne 0 ]]; then
		        echo "Failed to write cmd sudo $VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_NXT_64MB"
		        exit 1;
		fi
	} done
	ddr_dump_from=$ddr_dump_addr_tmp
	while true;
	do
		if (($ddr_dump_size_tmp < $window_size)); then
			size=$ddr_dump_size_tmp;
		else
			size=$window_size
		fi
		echo "---------------------------------"
		echo "DDR dump from 0x$( printf "%x" $ddr_dump_from) to 0x$( printf "%x" $(($ddr_dump_from+$size-1)))"
		rm -rf ddr_dump_t.bin
		$VKCLI $dev_id rf $PCIE_BAR2 $window_start_offset $size ddr_dump_t.bin
		check_status "DDR DUMP"
		window_size=$DUMP_SIZE_64MB;
		window_start_offset=0;
		ddr_dump_size_tmp=$(($ddr_dump_size_tmp-$size));
		sync;
		cat ddr_dump_t.bin >> ${file_name}_${FE}.bin
		ddr_dump_from=$((ddr_dump_from+$size))
		if (( $ddr_dump_size_tmp == 0)); then
			echo "---------------------------------"
			break;
		fi
		# Request card to remap the pcie BAR2 to the next 64MB
		echo "Remap the pcie BAR2 to the next 64MB to read from offset 0x$( printf "%x" $ddr_dump_from)"
		$VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_NXT_64MB
		check_status "REMAP"
		#Check command status
		result=$(wait_cmd_to_fnish "$VKCLI $dev_id rb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET")
		if [[ $result -ne 0 ]]; then
		        echo "Failed to write cmd sudo $VKCLI $dev_id wb $PCIE_BAR0 $FW_STATUS_ADDR_OFFSET $CMD_REMAP_NXT_64MB"
		        exit 1;
		fi
	done
	rm -rf ddr_dump_t.bin
	sync
}

if (( $ddr_dump_addr )); then
	echo "Dumping DDR range from $ddr_dump_addr and size $ddr_dump_size"
	#dump_ddr_range $ddr_dump_addr $ddr_dump_size ddr_dump_${ddr_dump_addr}_${ddr_dump_size}
	dump_ddr_range $ddr_dump_addr $ddr_dump_size ddr_dump_`printf '%x' ${ddr_dump_addr}`_`printf '%x' $((${ddr_dump_addr}+${ddr_dump_size}-1))`
fi

if (( $dump_all)) || (($ddr_mve_queue)); then
	echo "Dumping ddr mve queue from offset $DDR_MVE_QUEUE_START_ADDR and size $DDR_MVE_QUEUE_SIZE"
	dump_ddr_range $DDR_MVE_QUEUE_START_ADDR $DDR_MVE_QUEUE_SIZE ddr_mve_queue_dump
fi

echo "RAMDUMP DONE"
echo "To reset the card execute: ${VKCLI} <dev_num> reset"
