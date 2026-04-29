#!/bin/sh

sudo insmod ./nvmev.ko \
memmap_start=20G \
memmap_size=43009M \
cpus=7,8
