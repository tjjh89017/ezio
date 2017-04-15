#!/bin/bash

# NOTE you should install pip, virtualenv, virtualenvwarpper first
# sudo apt install python3-pip
# sudo pip3 install virtualenv virtualenvwarpper

cd scripts/

export WORKON_HOME=~/Env
source /usr/local/bin/virtualenvwrapper.sh
mkvirtualenv ezio
pip3 install -r requirements.txt

# Run python script for OpenStack
