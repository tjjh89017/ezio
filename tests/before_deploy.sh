#!/bin/bash

SSH_ARGS="-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no"

# copy all stuff to remote server
scp $SSH_ARGS "tests/test_ezio.sh" "ezio@ezio-arm.kojuro.date:~/"
scp -r $SSH_ARGS "." "ezio@ezio-arm.kojuro.date:~/ezio/"
