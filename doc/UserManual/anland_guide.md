<!--
title: Anland User Guide
section: Guides
order: 3
desc: Deploy Anland on Android and connect to an Anland-enabled Wayland desktop in PRoot, chroot, or Droidspaces.
keywords: anland, wayland, android, proot, chroot, droidspaces, kwin, mutter, kde, gnome, gpu, adreno, snapdragon
-->

# Anland User Guide

> [中文版](anland_guide_zh.md)

Anland is a Wayland display solution for Android, not a Linux container manager. It lets an adapted Linux Wayland compositor render into shared GPU buffers. The Anland Android app then displays those buffers and forwards touch, keyboard, mouse, clipboard, audio, and other data.

A complete Anland display path has three components:

| Component | Runs on | Purpose |
| --- | --- | --- |
| Anland Android app (consumer) | Android | Allocates and displays buffers and receives Android input. |
| `display_daemon` | Android host | Connects the Android app and Linux compositor through a Unix domain socket. |
| Anland-enabled compositor (producer) | Linux environment | Uses the Anland backend to render desktops such as KDE Plasma and GNOME. |

PRoot, chroot, and Droidspaces are different ways to host a Linux user space. Whichever one you choose, the consumer and producer must ultimately connect to the same `display_daemon`.

### Quick navigation

- [Choose a runtime](#runtime)
- [Requirements](#requirements)
- [Step 1: Install the Android side](#android-side)
- [Step 2: Connect the Linux environment](#linux-environment)
  - [chroot](#chroot)
  - [Droidspaces](#droidspaces)
- [Step 3: Install the Anland producer](#producer)
  - [Build from source](#source-build)
  - [Use prebuilt packages or a RootFS](#prebuilt)
- [Step 4: Start the desktop](#start-desktop)
- [Troubleshooting](#troubleshooting)

---

<a id="runtime"></a>

## Choose a runtime

| Runtime | Root required | Covered here |
| --- | :---: | --- |
| PRoot / native Termux environment | No | Uses the separate Anland: Termux project and its external guide. |
| chroot | Yes | This guide covers the standard Anland app, host daemon, socket mapping, and producer setup. |
| Droidspaces | Yes | Uses the built-in per-container Anland daemon and separate **Anland Display** switch, with no standalone daemon module or manual socket mapping. |

### PRoot

PRoot users should follow the [Anland: Termux User Guide](https://github.com/lfdevs/anland-termux/blob/main/docs/user-guide.md). That project provides its own Android app, Termux daemon, PRoot image, and startup scripts, and it also supports devices without root.

> [!IMPORTANT]
>
> Anland: Termux uses a different APK, daemon, and socket layout from the standard Anland path described below. Follow its external guide from start to finish for PRoot; do not mix the two installation procedures.

The rest of this page covers standard Anland deployments in chroot and Droidspaces only.

---

<a id="requirements"></a>

## Requirements

Before you begin, confirm the following:

1. **Android 11 or later with root access**: the chroot path runs a standalone `display_daemon`; Droidspaces manages the daemon through its built-in integration. The Anland app will normally still use its root helper to connect to the corresponding socket.
2. **An ARM64 device and Linux environment**: the current APK, daemon, and available producer builds target AArch64/ARM64.
3. **An Anland-enabled compositor**: the stock KWin or Mutter packages in normal distributions do not include the Anland backend. You must install a patched version supplied by the project.
4. **Working GPU devices and drivers**: Snapdragon/Adreno devices are recommended. The Linux environment must expose `/dev/dri/renderD128` correctly and provide the corresponding KGSL/Mesa drivers. Support on other platforms depends on their drivers; they may be limited to software rendering or unable to start the desktop.
5. **Compatible component versions**: an Anland protocol update may require updating both the Android app and producer. A standalone daemon used with chroot should also come from a similar release period. Do not arbitrarily mix components from widely separated versions.

> [!NOTE]
>
> Anland only bridges display, input, and related data. It does not create the Linux environment, install a complete desktop, configure chroot mounts, or provide GPU drivers.

---

<a id="android-side"></a>

## Step 1: Install the Android side

Both chroot and Droidspaces users must first download and install `anland-v5.apk` from the [latest Anland release](https://github.com/superturtlee/anland/releases/latest), then complete the setup for their chosen runtime.

### Android side for chroot

1. Install Anland's standalone `display_daemon` module. Download the artifact named `anland-daemon` from the latest successful [Build APK and Daemon](https://github.com/superturtlee/anland/actions/workflows/build.yml) run, then install the downloaded ZIP with a root manager that supports the Magisk module format.
2. Restart the Android device, then confirm from a root shell that the default socket was created:

   ```sh
   su -c 'test -S /data/local/tmp/display_daemon.sock && echo "Anland daemon is ready"'
   ```

3. Open Anland Settings and confirm these connection options:

   - **Daemon socket path**: keep the default `/data/local/tmp/display_daemon.sock`.
   - **Connect with root (helper)**: keep this enabled and grant Anland permission in your root manager.

> [!NOTE]
>
> Some releases provide only the APK and do not include the daemon module. In that case, obtain `anland-daemon` from a successful Actions build from the same period. If you also use the APK artifact, prefer downloading `anland-v5` and `anland-daemon` from the same run.

### Android side for Droidspaces

The Droidspaces [`anland` branch](https://github.com/ravindu644/Droidspaces-OSS/tree/anland) already includes built-in integration and is planned for merging into `main`. The following instructions apply to a Droidspaces build with a separate **Anland Display** switch. After the feature is merged, use a `main` build that includes it.

- Install only the Anland APK; do not install the separate `anland-daemon` module. Droidspaces starts an independent daemon for every container with Anland enabled.
- Grant root access when Anland requests it, but do not enter the container socket manually. Droidspaces passes the current container's dynamic socket path directly to Anland through **Launch Anland**.
- Do not connect a Droidspaces container by opening the Anland launcher icon. The launcher uses the global default socket, while Droidspaces uses a separate per-container socket.

For a complete description of the app settings, see the [Anland Settings Guide](anland_settings_guide.md).

---

<a id="linux-environment"></a>

## Step 2: Connect the Linux environment

The Anland producer always connects to this path inside the Linux environment:

```text
/run/display.sock
```

For chroot, map the Android host's default socket to that path:

```text
/data/local/tmp/display_daemon.sock -> /run/display.sock
```

Droidspaces instead creates a per-container socket such as `anland-<UUID>.sock` under the host's `/data/local/tmp` directory and automatically bind-mounts it at `/run/display.sock` inside the container. You do not need to inspect, enter, or manually map this dynamic host path.

The startup scripts below connect to the daemon through `ANLAND_SOCKET=/run/display.sock`.

<a id="chroot"></a>

### chroot

This section assumes that you already have a working chroot and that your chroot setup mounts `/proc`, `/sys`, `/dev`, and `/dev/pts`. Anland does not replace these basic mounts.

1. In an Android root shell, change `CHROOT_ROOT` to the actual RootFS path, then map the daemon socket:

   ```sh
   CHROOT_ROOT=/data/local/chroot/debian

   test -S /data/local/tmp/display_daemon.sock || exit 1
   mkdir -p "$CHROOT_ROOT/run"
   touch "$CHROOT_ROOT/run/display.sock"
   mount --bind \
     /data/local/tmp/display_daemon.sock \
     "$CHROOT_ROOT/run/display.sock"
   ```

2. Make sure the GPU device nodes are accessible inside the chroot. Adreno devices normally require at least `/dev/dri/renderD128` and `/dev/kgsl-3d0`; your chroot startup script should handle the exact mounts and permissions.
3. Enter the chroot and check the socket and render node:

   ```sh
   test -S /run/display.sock
   test -e /dev/dri/renderD128
   ```

4. Install the Anland producer described in the next section, then start the desktop session as a normal Linux user.

The socket is a runtime file. After Android restarts or the daemon recreates its socket, the chroot startup script must recreate the bind mount.

<a id="droidspaces"></a>

### Droidspaces

First install the Anland APK and use a build from the Droidspaces `anland` branch that includes the separate **Anland Display** switch. Once the feature is merged, use the corresponding `main` build. Then edit the target container:

1. Enable **GPU Access**. Also enable **Hardware Access** if your RootFS or device explicitly requires full hardware mapping.
2. Enable the separate **Anland Display** switch.
3. Disable **Configure Termux:X11**. Anland and Termux:X11 are independent display paths; configuring both can cause environment-variable or service conflicts.
4. Start the container and check the automatically created socket and GPU node:

   ```sh
   test -S /run/display.sock
   test -e /dev/dri/renderD128
   ```

5. After starting the Anland producer, select **Launch Anland** on the running-container card or container details page. Some versions label this button `anland`. Droidspaces passes the container's dynamic socket path to Anland and opens a separate window.

> [!IMPORTANT]
>
> The **Anland Display** switch starts Droidspaces' built-in per-container daemon and automatically bind-mounts it at `/run/display.sock` inside the container. Do not add a manual mapping for `/data/local/tmp/display_daemon.sock`, and do not change Anland's global default path to a Droidspaces dynamic socket.

### Recommended Droidspaces settings

- **Keep Configure PulseAudio disabled**: the Anland producer connects to Anland's audio path through PipeWire, so Droidspaces does not need to configure PulseAudio separately. If the container was previously configured for PulseAudio, remove the existing `PULSE_SERVER` environment variable as well.
- **Prefer keeping SELinux enforcing**: the Droidspaces Anland integration includes the corresponding SELinux policy. Use permissive mode temporarily for troubleshooting only after confirming that an AVC denial is related to Anland.
- **Set privilege options as required by the RootFS**: some Droidspaces/RootFS combinations require `nocaps` or `noseccomp`. These are container compatibility settings, not requirements of the Anland protocol itself.
- **KDE Plasma on Debian/Ubuntu**: make sure the kernel has User Namespace support enabled. If Plasma is noticeably sluggish or input is delayed, then check the `noseccomp` setting required by your Droidspaces version.

---

<a id="producer"></a>

## Step 3: Install the Anland producer

The desktop in the Linux environment must use a compositor with an Anland backend. The current Anland source tree primarily maintains a KWin backend for KDE Plasma and a Mutter backend for GNOME.

<a id="source-build"></a>

### Build from source

This is the installation path provided by the Anland project itself. It is suitable when you need to inspect the patches, debug a backend, or build the packages yourself.

1. Clone the source inside the chroot or Droidspaces container:

   ```sh
   git clone https://github.com/superturtlee/anland.git
   cd anland
   ```

2. Select a directory for your desktop and distribution:

   | Desktop | Distribution | Directory |
   | --- | --- | --- |
   | KDE Plasma | Debian 13 | `producers/kde/Debian13_v5` |
   | KDE Plasma | Ubuntu 26.04 | `producers/kde/ubuntu2604_v5` |
   | KDE Plasma | Fedora 43 | `producers/kde/Fedora43_v5` |
   | KDE Plasma | Arch Linux | `producers/kde/Arch_v5` |
   | GNOME | Debian 13 | `producers/gnome/Debian13_v5` |
   | GNOME | Ubuntu 26.04 | `producers/gnome/Ubuntu2604_v5` |

3. Enter the selected directory and run `build.sh`. For example, for KDE on Debian 13:

   ```sh
   cd producers/kde/Debian13_v5
   ./build.sh
   ```

The build script installs build dependencies, obtains the distribution sources, applies the Anland patches, builds packages, and installs the results. It invokes `sudo` when needed, so make sure the current user has sudo access. Building KWin, Mutter, and Xwayland takes considerable time and storage space.

<a id="prebuilt"></a>

### Use prebuilt packages or a RootFS

If you do not want to compile on the device, you can use third-party prebuilt packages or a RootFS with the producer already integrated.

[Droidspaces RootFS Desktop Builder](https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder) provides:

- KDE installers for Debian 13, Ubuntu 26.04, Fedora 43/44, and Arch Linux ARM64.
- GNOME installers for Debian 13 and Ubuntu 26.04 ARM64.
- Ready-to-use Droidspaces RootFS images with the Anland producer integrated, which you can also build yourself through GitHub Actions.

#### Use a packaged RootFS directly

This is the simplest option. You do not need to prepare a Linux system first or run `build.sh` or a producer installer on the device:

1. Open the project's [Releases](https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder/releases), download a `.tar.xz` RootFS marked `Wayland`, and choose a KDE, KDE Mobile, or GNOME desktop as needed.
2. Import the downloaded RootFS directly into Droidspaces.
3. Follow the [Droidspaces configuration](#droidspaces) in this guide to enable GPU access and the separate **Anland Display** switch. Droidspaces manages the daemon and `/run/display.sock` mapping automatically.
4. Start the container and continue directly to [Step 4: Start the desktop](#start-desktop). The packaged RootFS already includes the patched compositor, Anland environment configuration, and the corresponding desktop startup command; you do not need to install the producer again.

An example filename is:

```text
Ubuntu-26-kde-Wayland-Droidspaces-rootfs-aarch64-v20260702-120000.tar.xz
```

#### Install prebuilt packages in an existing Linux environment

You can run the project's installers separately in an existing Linux environment. First clone the project:

```sh
git clone https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder.git
cd Droidspaces-rootfs-Desktop-builder
```

KDE Plasma users should run:

```sh
sudo ./scripts/install-anland-kde.sh
```

GNOME users should run:

```sh
sudo ./scripts/install-anland-gnome.sh
```

These installers and RootFS images are release artifacts maintained by an external project; they are not part of an Anland release. Before using them, confirm that your distribution, version, and architecture match the project's support matrix.

---

<a id="start-desktop"></a>

## Step 4: Start the desktop

The `startup.sh` scripts in the source tree set the main environment variables required by Anland. For the default socket, run the following from your selected producer directory:

```sh
./startup.sh /run/display.sock
```

The script normally sets:

```text
ANLAND=1
ANLAND_SOCKET=/run/display.sock
ANLAND_DRM_DEVICE=/dev/dri/renderD128
```

If you use a prebuilt RootFS, prefer the desktop startup command it provides. For example, the KDE environment from Droidspaces RootFS Desktop Builder can be started with:

```sh
startplasma-wayland
```

After starting the producer:

- chroot users should open or switch back to the Anland Android app.
- Droidspaces users should select **Launch Anland** on the running-container card or container details page. Do not open the Anland launcher icon directly; only the Droidspaces action passes the container's dynamic socket path to Anland.

The desktop appears automatically after both the consumer and producer have connected.

> [!IMPORTANT]
>
> Do not start the session with your distribution's unpatched `kwin_wayland` or Mutter. A normal Wayland compositor will not submit frames to Anland even if it starts successfully.

---

<a id="troubleshooting"></a>

## Troubleshooting

### Anland opens only Settings or reports that it cannot connect

For chroot, first check the default host socket from an Android root shell:

```sh
su -c 'test -S /data/local/tmp/display_daemon.sock'
```

If it does not exist, check that the `anland-daemon` module is enabled and the device has been restarted. If the socket exists, confirm that the path in Anland Settings is correct, **Connect with root (helper)** is enabled, and root permission has not been denied.

Droidspaces users should not check or enter this global default socket. Confirm that:

1. You are using an `anland` branch build with the separate **Anland Display** switch, or a `main` build after that feature is merged.
2. **Anland Display** is enabled for the target container and the container is running.
3. `/run/display.sock` exists inside the container.
4. You opened the window through Droidspaces' **Launch Anland** action or `anland` button, rather than the Anland launcher icon.

If a running container has no Anland launch button, the current Droidspaces version usually does not include the integration, or the container's per-container daemon/socket failed to start.

### The app connects but no desktop appears

Check inside the Linux environment:

```sh
test -S /run/display.sock
printf '%s\n' "$ANLAND_SOCKET"
```

Confirm that you started an Anland-patched KWin or Mutter, then inspect the producer's terminal output. The most common causes are a socket that was not mapped into the Linux environment, an incorrect `ANLAND_SOCKET`, or a system update that replaced the patched packages.

### The desktop fails to start or uses only software rendering

Confirm that the Linux environment can access `/dev/dri/renderD128`. On an Adreno device, also check that the KGSL node and matching Mesa drivers are available. Droidspaces users should recheck GPU/Hardware Access settings; chroot users should check device-node mounts and user permissions.

### KDE Plasma is noticeably sluggish

On Debian or Ubuntu, first confirm that User Namespace support is enabled in the kernel. With Droidspaces, also check the `noseccomp` setting required by your version. Do not leave SELinux in permissive mode permanently; inspect the AVC logs first and fix the specific permission.

### You need to adjust input, resolution, audio, or window behavior

See the [Anland Settings Guide](anland_settings_guide.md). Settings are normally saved immediately, but connection, audio, and output-resolution options take effect on the next connection.
