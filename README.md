# GPU Screen Recorder Notification
Notification in the style of ShadowPlay.

# Dependencies
GPU Screen Recorder overlay uses meson build system so you need to install `meson` to build GPU Screen Recorder overlay.

## Build dependencies
These are the dependencies needed to build GPU Screen Recorder Notification:

* x11 (libx11, libxrandr, libxrender, libxfixes)
* libglvnd (which provides libgl, libglx and libegl)

## Runtime dependencies
There are also additional dependencies needed at runtime:

* Noto fonts

# Installation
Run `sudo ./install.sh`. This will install gsr-notify to `/usr/bin/gsr-notify`. You can run meson commands manually to install gsr-notify to another directory.
