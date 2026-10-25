# SPDX-License-Identifier: GPL-3.0-or-later
# platform/host/fat (OS-QANET D1): the simulator's card on a FAT32 IMAGE FILE, through the unit's own FatFs code. FatFsStorage (fat_fs_storage.h) is a hal::storage::Storage and StorageWriter
# made of platform/mk66's FatStorage and FatStorageWriter (platform_mk66_pure, which carries third_party/fatfs with the unit's ffconf.h) over an image device; FatImage (fat_image.h) writes a FAT32
# volume from a folder (the tool tests/tools/o3fatimg.cpp, and `card copy` on an image). A library of its own so that platform_host, which every host test links, does not pull the unit's file system in.
# Host only; never linked by core..app.
o3os_library(platform_host_fat SOURCES src/fat_image.cpp src/fat_fs_storage.cpp DEPENDS platform_host platform_mk66_pure)
