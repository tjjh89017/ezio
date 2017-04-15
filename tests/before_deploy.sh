#!/bin/bash

SSH_ARGS="-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no"
EZIO="static-ezio"

# copy all stuff to remote server
scp $SSH_ARGS "$EZIO" "ezio@ezio.kojuro.date:~/"
scp $SSH_ARGS "tests/test_ezio.sh" "ezio@ezio.kojuro.date:~/"
scp $SSH_ARGS "tests/ezio-openrc.sh" "ezio@ezio.kojuro.date:~/"
scp $SSH_ARGS "tests/ezio.pem" "ezio@ezio.kojuro.date:~/"
scp -r $SSH_ARGS "tests/scripts/" "ezio@ezio.kojuro.date:~/"
