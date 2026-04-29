#!/bin/sh

sudo mkfs.f2fs -f -m -c /dev/nvme4n1 /dev/nvme0n1p1

sleep 1

sudo mount -t f2fs /dev/nvme0n1p1 /mnt/f2fs/

exit 0
