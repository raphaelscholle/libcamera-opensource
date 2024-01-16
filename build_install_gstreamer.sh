#bin/bash

sudo DESTDIR=/opt/libcamera-openhd/ ninja -C build install -j 4

sudo cp /opt/libcamera-openhd/usr/lib/arm-linux-gnueabihf/gstreamer-1.0/libgstlibcamera.so /usr/lib/arm-linux-gnueabihf/gstreamer-1.0/


sudo ldconfig

