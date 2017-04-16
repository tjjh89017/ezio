#!/bin/bash

# NOTE you should install pip, virtualenv, virtualenvwarpper first
# sudo apt install python3-pip
# sudo pip3 install virtualenv virtualenvwarpper

source ezio-openrc.sh

# ssh key
chmod 600 ezio.pem
eval $(ssh-agent)
ssh-add ezio.pem

# Run python script for openstack
scripts/main.py
