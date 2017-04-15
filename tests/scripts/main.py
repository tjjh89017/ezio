#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import time

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

def main():

    # get config
    ses = get_session()

    # login openstack
    nova = Client("2.1", session=ses, insecure=True)

    # create 10 VM
    flavor = nova.flavors.find(name='m1.small')
    image = nova.glance.find_image('Ubuntu 16.04 x86_64')
    key_name = 'ezio'
    min_count = 5

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

    # copy stuff to instances

    # run script

    # collect the result

    # delete instances
    list(map(nova.servers.delete, nova.servers.list()))

    pass

if __name__ == '__main__':
    main()
