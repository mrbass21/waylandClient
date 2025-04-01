#!/bin/bash

gcc -o client client.c xdg-shell-protocol.c -DUSE_SHM -lwayland-client