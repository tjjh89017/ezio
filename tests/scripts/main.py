#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import time
from subprocess import Popen, PIPE

from keystoneauth1.identity import v3
from keystoneauth1 import session
from novaclient.client import Client

def get_session():
    auth = v3.Password(
        auth_url=os.environ['OS_AUTH_URL'],
        username=os.environ['OS_USERNAME'],
        password=os.environ['OS_PASSWORD'],
        project_name=os.environ['OS_PROJECT_NAME'],
        user_domain_name='local',
        project_domain_name='local',
    )
    return session.Session(auth=auth, verify=False)

def ssh(user, ip, cmd, key=None):

    # SSH_ARGS="-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no"
    ssh_cmd = ['ssh', '-o', 'UserKnownHostsFile=/dev/null', '-o', 'StrictHostKeyChecking=no']
    if key:
        ssh_cmd = ssh_cmd + ['-i', key]
    ssh_cmd.append('%s@%s' % (user, ip))
    ssh_cmd = ssh_cmd + cmd

    process = Popen(ssh_cmd, stdout=PIPE, stderr=PIPE)
    return process

def scp(user, ip, filename, key=None):

    scp_cmd = ['scp', '-o', 'UserKnownHostsFile=/dev/null', '-o', 'StrictHostKeyChecking=no']
    if key:
        scp_cmd = scp_cmd + ['-i', key]
    scp_cmd.append(filename)
    scp_cmd.append('%s@%s:./' % (user, ip))
    process = Popen(scp_cmd, stdout=PIPE, stderr=PIPE)
    return process.wait()

def wait_for_instance(user, ip, key=None):

    while True:
        process = ssh(user, ip, ['true'], key)
        if process.wait() == 0:
            break
        

def main():

    # get config
    ses = get_session()

    # login openstack
    nova = Client("2.1", session=ses, insecure=True)

    # create 10 VM
    flavor = nova.flavors.find(name='m1.medium')
    image = nova.glance.find_image('ezio')
    key_name = 'ezio'
    min_count = 10

    nova.servers.create("ezio", image, flavor, key_name=key_name, min_count=min_count)

    while True:
        time.sleep(10)
        if len(nova.servers.list(search_opts={'status': 'ACTIVE'})) >= min_count:
            break
    l = list(map(lambda x : x.networks, nova.servers.list(search_opts={'status': 'ACTIVE'})))
    ips = []
    for x in l:
        for y in x.values():
            ips = ips + y
    print(ips)

    # wait for ssh
    print("wait for ssh")
    for ip in ips:
        wait_for_instance('ubuntu', ip)

    # copy stuff to instances
    print("scp stuff to instances")
    for ip in ips:
        scp('ubuntu', ip, 'a.torrent')
        scp('ubuntu', ip, 'static-ezio')

    # run script
    print("run script")
    procs = []
    for ip in ips:
        process = ssh('ubuntu', ip, ['./static-ezio', '-t', '1', 'a.torrent', '/tmp/target'])
        procs.append((ip, process))

    # collect the result
    print("wait for result")
    for ip, process in procs:
        output = process.communicate()[0]
        #output = output.replace('\r', '\n')
        print(ip)
        print(output)

    # delete instances
    list(map(nova.servers.delete, nova.servers.list()))

if __name__ == '__main__':
    main()
