#!/bin/bash

SSH_ARGS="-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no"
EZIO="static-ezio"

# ssh to ezio.kojuro.date and run script
scp $SSH_ARGS "$EZIO" "ezio@ezio.kojuro.date:~/"
ssh $SSH_ARGS "ezio@ezio.kojuro.date" ./test_ezio.sh
