#!/bin/sh
 
# udhcpc script edited by Tim Riker <Tim@Rikers.org>
 
[ -z "$1" ] && echo "Error: should be called from udhcpc" && exit 1
 
RESOLV_CONF="/etc/resolv.conf"
[ -n "$broadcast" ] && BROADCAST="broadcast $broadcast"
[ -n "$subnet" ] && NETMASK="netmask $subnet"
[ -n "$siaddr" ] && echo $siaddr > /tftp
 
case "$1" in
  deconfig)
	busybox ifconfig $interface 0.0.0.0
	;;
 
  renew|bound)
	busybox ifconfig $interface $ip $BROADCAST $NETMASK
 
	if [ -n "$router" ] ; then
	  echo "deleting routers"
	  while busybox route del default gw 0.0.0.0 dev $interface ; do
		:
	  done
 
	  for i in $router ; do
		busybox route add default gw $i dev $interface
	  done
	fi
 
	echo -n > $RESOLV_CONF
	[ -n "$domain" ] && echo search $domain >> $RESOLV_CONF
	for i in $dns ; do
	  echo adding dns $i
	  echo nameserver $i >> $RESOLV_CONF
	done
	;;
esac
 
exit 0
