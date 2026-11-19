#!/bin/sh
# Run on the Pi after install.sh --dev (which installs stick-gadget and mkfs.vfat): make the dev stick, a 2 GB FAT32
# image with vagent's job folders, and connect it to the car.
set -e
if [ ! -f /srv/stick.img ]; then
    sudo truncate -s 2G /srv/stick.img
    sudo mkfs.vfat -F 32 -n VELOSTERAA /srv/stick.img >/dev/null
    sudo mkdir -p /mnt/stick
    sudo mount -o loop,uid="$(id -u)" /srv/stick.img /mnt/stick
    echo "velosterAA test stick, created $(date -Is)" > /mnt/stick/README_VELOSTERAA.txt
    mkdir -p /mnt/stick/vagent/jobs /mnt/stick/vagent/results
    sudo umount /mnt/stick
fi
sudo stick-gadget up
sleep 4
sudo stick-gadget status
dmesg | grep -iE 'dwc2|gadget|mass_storage|configfs' | tail -6
