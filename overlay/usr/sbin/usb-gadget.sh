#!/bin/sh
# USB gadget: RNDIS network (usb0) + ACM serial console (ttyGS0)
# (NCM in the Samsung kernel never reports carrier to the host)
set -e

CFS=/sys/kernel/config
G=$CFS/usb_gadget/g1

mountpoint -q $CFS || mount -t configfs none $CFS

UDC=""
for i in $(seq 100); do
	UDC=$(ls /sys/class/udc 2>/dev/null | head -n1)
	[ -n "$UDC" ] && break
	sleep 0.2
done
[ -n "$UDC" ] || { echo "no UDC found" >&2; exit 1; }

# already bound by the initramfs (early shell): keep it, the console is in use
[ -n "$(cat $G/UDC 2>/dev/null)" ] && exit 0

mkdir -p $G
cd $G

echo 0x1d6b > idVendor
echo 0x0104 > idProduct
mkdir -p strings/0x409
echo t510 > strings/0x409/serialnumber
echo Ubuntu > strings/0x409/manufacturer
echo "Galaxy Tab A 10.1 (Ubuntu)" > strings/0x409/product

mkdir -p functions/rndis.usb0 functions/acm.GS0
# fixed MACs so the PC recognizes the tablet on every boot
echo 02:00:00:51:05:01 > functions/rndis.usb0/dev_addr
echo 02:00:00:51:05:10 > functions/rndis.usb0/host_addr
mkdir -p configs/c.1/strings/0x409
echo "RNDIS + ACM" > configs/c.1/strings/0x409/configuration
echo 500 > configs/c.1/MaxPower
[ -e configs/c.1/rndis.usb0 ] || ln -s functions/rndis.usb0 configs/c.1/
[ -e configs/c.1/acm.GS0 ] || ln -s functions/acm.GS0 configs/c.1/

echo "$UDC" > UDC
