#!/bin/sh

# Get tftp server from $siaddr
TFTP=$siaddr

# download config file
busybox tftp -g -l ezio.conf -r ezio.conf $TFTP

# parsing config file
# TODO using better way to do this

# torrent
TORRENT=$(cat ezio.conf | grep 'torrent' | sed 's|.*=[_[:blank:]]*||')
# get file
[ -n "$TORRENT" ] && busybox tftp -g -l $TORRENT -r $TORRENT $TFTP

# magnet uri
MAGNET=$(cat ezio.conf | grep 'magnet' | sed 's|.*=[_[:blank:]]*||')

# target disk
TARGET=$(cat ezio.conf | grep 'disk' | sed 's|.*=[_[:blank:]]*||')

# exec ezio
[ -n "$TORRENT" ] && TORRENT=$MAGNET
static-ezio $TORRENT $TARGET

echo "Clone done. Shutdown."
poweroff
