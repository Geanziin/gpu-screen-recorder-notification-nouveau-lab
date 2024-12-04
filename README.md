# GPU Screen Recorder Notification
Notification in the style of ShadowPlay.


On X11 the notification shows on the monitor that the cursor is on. On Wayland the notification shows on the monitor the Wayland compositor considers focused.

# Installation
If you are using an Arch Linux based distro then you can find gpu screen recorder notification on aur under the name gpu-screen-recorder-notification (`yay -S gpu-screen-recorder-notification`).\
If you are running another distro then you can run `sudo ./install.sh`, but you need to manually install the dependencies, as described below.

# Dependencies
GPU Screen Recorder overlay uses meson build system so you need to install `meson` to build GPU Screen Recorder notification.

## Build dependencies
These are the dependencies needed to build GPU Screen Recorder Notification:

* x11 (libx11, libxrandr, libxrender, libxext)
* libglvnd (which provides libgl, libglx and libegl)

# License
This software is licensed under GPL3.0-only. `fonts/NotoSans-Bold.ttf` is licensed under `SIL Open Font License`.

# Screenshots
![](https://dec05eba.com/images/gsr-notify-screenshot.png)
