# GPU Screen Recorder Notification
Notification in the style of ShadowPlay.

On X11 the notification shows on the monitor that the cursor is on. On Wayland the notification shows on the monitor the Wayland compositor considers focused.

# Installation
If you are running an Arch Linux based distro then you can find gpu screen recorder notification in the official repositories under the name gpu-screen-recorder-notification (`sudo pacman -S gpu-screen-recorder-notification`).\
If you are running another distro then you can run `sudo ./install.sh`, but you need to manually install the dependencies, as described below.

# Dependencies
GPU Screen Recorder overlay uses meson build system so you need to install `meson` to build GPU Screen Recorder notification.

## Build dependencies
These are the dependencies needed to build GPU Screen Recorder Notification:

* x11 (libx11, libxrandr, libxrender, libxext)
* libglvnd (which provides libgl, libglx and libegl)
* wayland (wayland-client, wayland-egl, wayland-scanner)
* libpango (pangoft2)

# License
This software is licensed under GPL3.0-only.

# Screenshots
![](https://dec05eba.com/images/gsr-notify-screenshot.png)
