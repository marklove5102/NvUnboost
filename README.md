# NvUnboost

### Introduction

This small tool provides an experimental workaround for application/system freezes caused by a combination of Windows 11 24H2 and NVIDIA display drivers. It is not endorsed by Microsoft or NVIDIA. Use it at your own risk.

For more details, refer to this post on the NVIDIA forums: [Threads getting stuck on real-time priority on Windows 11 24H2](https://www.nvidia.com/en-us/geforce/forums/game-ready-drivers/13/561084/threads-getting-stuck-on-real-time-priority-on-win/)

It continuously monitors all threads on the system, and attempts to lower the priority of threads that appear to be stuck at a real-time dynamic priority level (16 or higher), but with a base priority lower than 16.

Note that it might not be able to fix threads in processes running as administrator, unless NvUnboost itself is running as administrator. Even then, it might not have access to some protected/system threads, which is usually indicated by the "OpenThread failed (0x5)" message in the "Comment" column. This is probably fine, as some of those threads may belong to drivers and may be intentionally running at a higher priority. For example, such cases can be observed in nvcontainer.exe even on Windows 11 23H2, where it doesn't appear to cause any issues.

A maximum of 1000 events are reported in the main window. The oldest events above this limit are dropped.

Minimizing the window removes it from the taskbar. It can be restored by clicking on its system tray icon. Close the window to terminate the application and stop monitoring/fixing threads.

### Installation

No installation required. Just download the latest [release](https://github.com/narzoul/NvUnboost/releases), extract the zip file and run NvUnboost.exe from any directory.

### Configuration

There are no configuration files. The application itself doesn't create any files either, nor writes anything to the registry.

However, the `/startminimized` command-line argument can be used to start it in a minimized state.

### License
Licensed under the [BSD Zero Clause License](LICENSE.txt).
