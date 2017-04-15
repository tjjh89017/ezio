#!/bin/bash

SSH_ARGS="-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no"

# ssh to ezio.kojuro.date and run script
ssh $SSH_ARGS "ezio@ezio.kojuro.date" scripts/test_ezio.sh
