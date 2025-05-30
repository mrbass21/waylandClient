#!/bin/bash

gcc -g -o client client.c xdg-shell-protocol.c xdg-decoration.c -DUSE_SHM -lwayland-client
