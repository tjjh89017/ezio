#!/bin/sh -xe
cd ${PWD%scripts*}
./scripts/build_ezio.sh
./scripts/build_pxe.sh

echo "====================================================="
echo "Done."
echo "Remember to give an ezio.conf to tftp root directory."
echo "====================================================="
