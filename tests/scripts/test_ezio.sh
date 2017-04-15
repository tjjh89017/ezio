#!/bin/bash

# NOTE you should install pip, virtualenv, virtualenvwarpper first
# sudo apt install python3-pip
# sudo pip3 install virtualenv virtualenvwarpper
export WORKON_HOME=~/Env
source /usr/local/bin/virtualenvwarpper.sh
mkvirtualenv ezio
pip3 install -r requirements.txt

# Run python script for OpenStack
