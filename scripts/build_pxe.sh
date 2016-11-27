#!/bin/bash -xe

apt update
apt install tftpd-hpa isc-dhcp-server -y
apt install pxelinux syslinux -y
Interface=$(ip route | awk '/^default/{print $5}')
Gateway=$(ip route | awk '/^default/{print $3}')
IPv4=$(ifconfig $Interface | awk '/^ *inet /{print $2}' | cut -f2 -d':')
Boardcast=$(ifconfig $Interface | awk '/^ *inet /{print $3}' | cut -f2 -d':')
Netmask=$(ifconfig $Interface | awk '/^ *inet /{print $4}' | cut -f2 -d':')
Nameserver=$(cat /etc/resolv.conf | grep '^nameserver' | head -n 1 | awk '{print $2}')
IFS=. read -r i1 i2 i3 i4 <<< "$IPv4"
IFS=. read -r m1 m2 m3 m4 <<< "$Netmask"
Subnet=$(printf "%d.%d.%d.%d\n" "$((i1 & m1))" "$((i2 & m2))" "$((i3 & m3))" "$((i4 & m4))")
echo \
"option routers          ${Gateway};
option broadcast-address        ${Boardcast};
option domain-name-servers      ${Nameserver};
subnet ${Subnet} netmask ${Netmask} {
        range ${Subnet%\.*}.101 ${Subnet%\.*}.200;
        option subnet-mask      ${Netmask};
        option domain-name      \"ezio\";
        next-server     ${IPv4}; 
        filename        \"pxelinux.0\"; 
}
" >> /etc/dhcp/dhcpd.conf
systemctl restart isc-dhcp-server
echo \
'TFTP_USERNAME="tftp"
TFTP_DIRECTORY="/tftp"
TFTP_ADDRESS="[::]:69"
TFTP_OPTIONS="--secure"
' > /etc/default/tftpd-hpa
systemctl restart tftpd-hpa

mkdir -p /tftp
cp /usr/lib/syslinux/modules/bios/* /tftp
cp /usr/lib/PXELINUX/pxelinux.0 /tftp
mkdir -p /tftp/pxelinux.cfg

echo \
"UI vesamenu.c32
TIMEOUT 300
DISPLAY ./boot.msg
MENU TITLE Welcome to Phd.ate's EZIO

LABEL ezio
  MENU LABEL Boot from EZIO Server for disk cloning
  MENU DEFAULT
  kernel ./kernel/linux
  append initrd=./kernel/initrd.img
  sysappend 2

LABEL local
  MENU LABEL Boot from local drive
  localboot 0
" > /tftp/pxelinux.cfg/default
chmod -R +r /tftp

mkdir -p /tftp/kernel
cp static-ezio /tftp/
cp utils/linux utils/initrd.img /tftp/kernel
