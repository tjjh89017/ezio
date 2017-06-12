#!/bin/sh

# Get tftp server from $siaddr
TFTP=$(cat /tftp)


# download config file
busybox tftp -g -l ezio.conf -r ezio.conf $TFTP

# parsing config file
while read line
do
    if echo $line | grep -F = &>/dev/null
    then
        varname=$(echo "$line" | cut -d '=' -f 1)
        export $varname=$(echo "$line" | cut -d '=' -f 2-)
    fi
done < ezio.conf

# torrent
TORRENT=$torrent
# get file
[ -n "$TORRENT" ] && busybox tftp -g -l $TORRENT -r $TORRENT $TFTP

# magnet uri
MAGNET=$magnet
[ -z "$TORRENT" ] && TORRENT=$MAGNET

# target disk
TARGET=$disk

# exec ezio
static-ezio "$TORRENT" "$TARGET"

echo "Clone done. Shutdown."
poweroff
