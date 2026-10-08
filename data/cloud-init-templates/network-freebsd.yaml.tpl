# nuageinit (FreeBSD 14.x/15.1) aborts when a match rule finds no interface,
# and driver names differ from Linux (vtnet vs. virtio_net). No interfaces are
# configured here; the cloud images bring ifconfig_DEFAULT="SYNCDHCP".
version: 2
ethernets: {}
