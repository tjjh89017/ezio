#!/bin/bash

# NOTE you should install pip, virtualenv, virtualenvwarpper first
# sudo apt install python3-pip
# sudo pip3 install virtualenv virtualenvwarpper

source ezio-openrc.sh

# ssh key
chmod 600 ezio.pem
eval $(ssh-agent)
ssh-add ezio.pem

# compile
cd ezio && make clean all
cd ..
cp ezio/ezio static-ezio

# Run python script for openstack
ezio/tests/scripts/main.py
