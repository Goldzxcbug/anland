<!--
title: Anland 使用指南
section: 指南
order: 3
desc: 在 Android 上部署 Anland，并连接 PRoot、chroot 或 Droidspaces 中支持 Anland 的 Wayland 桌面。
keywords: anland, wayland, android, proot, chroot, droidspaces, kwin, mutter, kde, gnome, gpu, adreno, snapdragon
-->

# Anland 使用指南

> [English](anland_guide.md)

Anland 是一套面向 Android 的 Wayland 显示方案，不是 Linux 容器管理器。它让经过适配的 Linux Wayland 合成器把画面渲染到共享 GPU 缓冲区，再由 Android 端的 Anland 应用显示画面并转发触摸、键盘、鼠标、剪贴板、音频等数据。

一条完整的 Anland 显示链路由三部分组成：

| 组件 | 运行位置 | 作用 |
| --- | --- | --- |
| Anland Android 应用（consumer） | Android | 分配并显示缓冲区，接收 Android 输入。 |
| `display_daemon` | Android 宿主 | 通过 Unix 域套接字连接 Android 应用与 Linux 合成器。 |
| 支持 Anland 的合成器（producer） | Linux 环境 | 使用 Anland 后端渲染 KDE Plasma、GNOME 等桌面。 |

PRoot、chroot 和 Droidspaces 只是承载 Linux 用户空间的不同方式。无论选择哪一种，最终都需要让 consumer 和 producer 连接到同一个 `display_daemon`。

### 快速导航

- [选择运行方式](#runtime)
- [前提条件](#requirements)
- [第 1 步：安装 Android 端](#android-side)
- [第 2 步：接入 Linux 环境](#linux-environment)
  - [chroot](#chroot)
  - [Droidspaces](#droidspaces)
- [第 3 步：安装 Anland producer](#producer)
  - [从源码构建](#source-build)
  - [使用预编译包或 RootFS](#prebuilt)
- [第 4 步：启动桌面](#start-desktop)
- [常见问题](#troubleshooting)

---

<a id="runtime"></a>

## 选择运行方式

| 运行方式 | 是否需要 Root | 本指南如何处理 |
| --- | :---: | --- |
| PRoot / Termux 原生环境 | 否 | 使用独立的 Anland: Termux 项目，跳转到其外部指南。 |
| chroot | 是 | 本指南说明标准 Anland 应用、宿主守护进程、socket 映射和 producer 配置。 |
| Droidspaces | 是 | 使用 Droidspaces 内置的每容器 Anland daemon 和独立 **Anland Display** 开关，无需安装独立 daemon 模块或手动映射 socket。 |

### PRoot

PRoot 用户请直接阅读 [Anland: Termux 中文用户指南](https://github.com/lfdevs/anland-termux/blob/main/docs/user-guide_zh.md)。该项目提供自己的 Android 应用、Termux 守护程序、PRoot 镜像和启动脚本，也支持无 Root 设备。

> [!IMPORTANT]
>
> Anland: Termux 与本页后续介绍的标准 Anland 路径使用不同的 APK、守护程序和 socket 布局。使用 PRoot 时请完整遵循外部指南，不要混用两套安装步骤。

本页余下内容只介绍标准 Anland 在 chroot 和 Droidspaces 中的部署。

---

<a id="requirements"></a>

## 前提条件

开始前请确认以下条件：

1. **Android 11 或更高版本，并已取得 Root 权限**：chroot 路径需要独立运行 `display_daemon`；Droidspaces 路径由其内置集成管理 daemon。Anland 应用通常仍会通过 Root 辅助程序连接对应 socket。
2. **ARM64 设备与 Linux 环境**：当前 APK、守护程序和现有 producer 构建面向 AArch64/ARM64。
3. **支持 Anland 的合成器**：普通发行版自带的 KWin 或 Mutter 不包含 Anland 后端，必须安装项目提供的补丁版本。
4. **可用的 GPU 设备和驱动**：推荐使用 Snapdragon / Adreno 设备，并在 Linux 环境中正确提供 `/dev/dri/renderD128` 以及对应的 KGSL/Mesa 驱动。其他平台的支持情况取决于驱动，可能只能使用软件渲染或无法启动桌面。
5. **版本相互兼容**：更新 Anland 协议版本后，Android 应用和 producer 也可能需要一起更新。chroot 使用的独立守护程序也应来自相近版本，不要随意混用年代相差较大的组件。

> [!NOTE]
>
> Anland 只负责显示与输入等桥接能力，不负责创建 Linux 环境、安装完整桌面、配置 chroot 挂载或提供 GPU 驱动。

---

<a id="android-side"></a>

## 第 1 步：安装 Android 端

chroot 和 Droidspaces 用户都需要先从 [Anland 最新 Release](https://github.com/superturtlee/anland/releases/latest) 下载并安装 `anland-v5.apk`，随后按运行方式完成对应配置。

### chroot 使用的 Android 端

1. 安装 Anland 的独立 `display_daemon` 模块。可以从 [Build APK and Daemon](https://github.com/superturtlee/anland/actions/workflows/build.yml) 的最新成功构建中下载名为 `anland-daemon` 的产物，并通过支持 Magisk 模块格式的 Root 管理器安装下载得到的 ZIP。
2. 重启 Android 设备，随后在 Root 终端确认默认 socket 已创建：

   ```sh
   su -c 'test -S /data/local/tmp/display_daemon.sock && echo "Anland daemon is ready"'
   ```

3. 打开 Anland 设置，确认以下连接选项：

   - **守护进程套接字路径**：保持默认的 `/data/local/tmp/display_daemon.sock`。
   - **使用 root 连接（辅助程序）**：保持开启，并在 Root 管理器中授予 Anland 权限。

> [!NOTE]
>
> 部分 Release 只提供 APK，没有同时附带守护进程模块。此时应从同一时期的成功 Actions 构建获取 `anland-daemon`；若同时使用构建产物中的 APK，优先从同一次构建下载 `anland-v5` 和 `anland-daemon`。

### Droidspaces 使用的 Android 端

Droidspaces 的 [`anland` 分支](https://github.com/ravindu644/Droidspaces-OSS/tree/anland)已经实现内置集成，并计划合并到 `main`。以下说明适用于带有独立 **Anland Display** 开关的 Droidspaces 构建；合并完成后可直接使用包含该功能的 `main` 版本。

- 只需安装 Anland APK，无需另外安装 `anland-daemon` 模块。Droidspaces 会为每个启用 Anland 的容器启动独立 daemon。
- Anland 请求 Root 权限时正常授予，但不要手动填写容器 socket。Droidspaces 的 **Launch Anland** 操作会把当前容器的动态 socket 路径直接传给 Anland。
- 不要通过 Anland 桌面图标连接 Droidspaces 容器；桌面图标使用的是全局默认 socket，而 Droidspaces 使用每容器独立 socket。

完整的应用设置说明见 [Anland 设置使用说明](anland_settings_guide_zh.md)。

---

<a id="linux-environment"></a>

## 第 2 步：接入 Linux 环境

Anland producer 在 Linux 环境中统一连接：

```text
/run/display.sock
```

chroot 路径需要把 Android 宿主的默认 socket 映射到这里：

```text
/data/local/tmp/display_daemon.sock -> /run/display.sock
```

Droidspaces 则会在宿主的 `/data/local/tmp` 下创建形如 `anland-<UUID>.sock` 的每容器 socket，并自动绑定到容器内的 `/run/display.sock`。用户不需要查看、填写或手动映射这个动态宿主路径。

后续启动脚本都通过 `ANLAND_SOCKET=/run/display.sock` 连接守护进程。

<a id="chroot"></a>

### chroot

本节假定你已经有一个可以正常进入的 chroot，并已按该 chroot 方案挂载 `/proc`、`/sys`、`/dev` 和 `/dev/pts`。Anland 不替代这些基础挂载。

1. 在 Android Root 终端中，把 `CHROOT_ROOT` 改成实际 rootfs 路径，然后映射 daemon socket：

   ```sh
   CHROOT_ROOT=/data/local/chroot/debian

   test -S /data/local/tmp/display_daemon.sock || exit 1
   mkdir -p "$CHROOT_ROOT/run"
   touch "$CHROOT_ROOT/run/display.sock"
   mount --bind \
     /data/local/tmp/display_daemon.sock \
     "$CHROOT_ROOT/run/display.sock"
   ```

2. 确保 chroot 内能够访问 GPU 节点。Adreno 设备通常至少需要 `/dev/dri/renderD128` 和 `/dev/kgsl-3d0`；具体挂载和权限应由你的 chroot 启动脚本处理。
3. 进入 chroot 后检查 socket 和渲染节点：

   ```sh
   test -S /run/display.sock
   test -e /dev/dri/renderD128
   ```

4. 安装下一节介绍的 Anland producer，并使用 Linux 普通用户启动桌面会话。

socket 是运行时文件，Android 重启或守护进程重建 socket 后，需要由 chroot 启动脚本重新建立绑定挂载。

<a id="droidspaces"></a>

### Droidspaces

先安装 Anland APK，并使用包含独立 **Anland Display** 开关的 Droidspaces `anland` 分支构建；该功能合并后则使用对应的 `main` 版本。随后编辑目标容器：

1. 启用 **GPU 访问**；若所用 RootFS 或设备明确要求完整硬件映射，则启用 **硬件访问**。
2. 启用独立的 **Anland Display** 开关。
3. 关闭 **配置 Termux:X11**。Anland 和 Termux:X11 是两条独立的显示链路，同时配置容易造成环境变量或服务冲突。
4. 启动容器后检查自动创建的 socket 和 GPU 节点：

   ```sh
   test -S /run/display.sock
   test -e /dev/dri/renderD128
   ```

5. 启动 Anland producer 后，在 Droidspaces 的运行中容器卡片或容器详情页点击 **Launch Anland**（部分界面显示为 `anland` 按钮）。Droidspaces 会把该容器的动态 socket 路径传给 Anland，并打开独立窗口。

> [!IMPORTANT]
>
> **Anland Display** 开关会启动 Droidspaces 内置的每容器 daemon，并自动将它绑定到容器内的 `/run/display.sock`。不要再添加 `/data/local/tmp/display_daemon.sock` 的手动绑定，也不要在 Anland 设置中把全局默认路径改成 Droidspaces 的动态 socket。

### Droidspaces 推荐设置

- **关闭配置 PulseAudio**：Anland producer 通过 PipeWire 接入 Anland 自带的音频链路，不需要 Droidspaces 额外配置 PulseAudio。若容器以前配置过 PulseAudio，还应删除已有的 `PULSE_SERVER` 环境变量。
- **优先保持 SELinux enforcing**：Droidspaces 的 Anland 集成包含对应的 SELinux 策略。只有在确认 AVC 拒绝与 Anland 有关时，才临时使用宽容模式排障。
- **按 RootFS 要求配置特权选项**：部分 Droidspaces/RootFS 组合需要 `nocaps` 或 `noseccomp`。这属于容器兼容设置，不是 Anland 协议本身的要求。
- **Debian / Ubuntu 的 KDE Plasma**：确保内核启用 User Namespace。若 Plasma 明显卡顿或操作延迟，再检查 Droidspaces 的 `noseccomp` 设置。

---

<a id="producer"></a>

## 第 3 步：安装 Anland producer

Linux 环境中的桌面必须使用带 Anland 后端的合成器。当前 Anland 源码树主要维护 KDE Plasma 的 KWin 后端和 GNOME 的 Mutter 后端。

<a id="source-build"></a>

### 从源码构建

这是 Anland 项目自身提供的安装路径，适合需要确认补丁内容、调试后端或自行构建软件包的用户。

1. 在 chroot 或 Droidspaces 容器中拉取源码：

   ```sh
   git clone https://github.com/superturtlee/anland.git
   cd anland
   ```

2. 根据桌面和发行版选择目录：

   | 桌面 | 发行版 | 目录 |
   | --- | --- | --- |
   | KDE Plasma | Debian 13 | `producers/kde/Debian13_v5` |
   | KDE Plasma | Ubuntu 26.04 | `producers/kde/ubuntu2604_v5` |
   | KDE Plasma | Fedora 43 | `producers/kde/Fedora43_v5` |
   | KDE Plasma | Arch Linux | `producers/kde/Arch_v5` |
   | GNOME | Debian 13 | `producers/gnome/Debian13_v5` |
   | GNOME | Ubuntu 26.04 | `producers/gnome/Ubuntu2604_v5` |

3. 进入对应目录并执行 `build.sh`。例如 Debian 13 KDE：

   ```sh
   cd producers/kde/Debian13_v5
   ./build.sh
   ```

构建脚本会安装编译依赖、获取发行版源码、应用 Anland 补丁、构建软件包并安装结果。它会在需要时调用 `sudo`，因此应确保当前用户具有 sudo 权限。构建 KWin、Mutter 和 Xwayland 需要较长时间和足够的存储空间。

<a id="prebuilt"></a>

### 使用预编译包或 RootFS

不想在设备上编译时，可以使用第三方维护的预编译包或已经集成 producer 的 RootFS。

[Droidspaces RootFS Desktop Builder](https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder) 提供：

- KDE 安装器：支持 Debian 13、Ubuntu 26.04、Fedora 43/44 和 Arch Linux ARM64。
- GNOME 安装器：支持 Debian 13 和 Ubuntu 26.04 ARM64。
- 已经集成 Anland producer 的成品 Droidspaces RootFS，也可以通过 GitHub Actions 自行构建。

#### 直接使用已经打包好的 RootFS

这是最省事的方式，不需要先准备 Linux 系统，也不需要在设备上运行 `build.sh` 或 producer 安装器：

1. 打开项目的 [Releases](https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder/releases)，下载带 `Wayland` 标记的 `.tar.xz` RootFS，并按需要选择 KDE、KDE Mobile 或 GNOME 桌面。
2. 将下载的 RootFS 直接导入 Droidspaces。
3. 按本指南的 [Droidspaces 配置](#droidspaces)启用 GPU 访问和独立的 **Anland Display** 开关；daemon 和 `/run/display.sock` 映射由 Droidspaces 自动管理。
4. 启动容器后直接进入 [第 4 步：启动桌面](#start-desktop)。成品 RootFS 已包含 patched 合成器、Anland 环境配置和对应的桌面启动命令，无需再次安装 producer。

文件名类似：

```text
Ubuntu-26-kde-Wayland-Droidspaces-rootfs-aarch64-v20260702-120000.tar.xz
```

#### 在现有 Linux 环境中安装预编译包

在现有 Linux 环境中可以单独运行其安装器。先拉取项目：

```sh
git clone https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder.git
cd Droidspaces-rootfs-Desktop-builder
```

KDE Plasma 用户运行：

```sh
sudo ./scripts/install-anland-kde.sh
```

GNOME 用户改为运行：

```sh
sudo ./scripts/install-anland-gnome.sh
```

安装器和 RootFS 属于外部项目维护的发行产物，不是 Anland Release 的一部分。使用前应确认发行版、版本和架构与其支持矩阵一致。

---

<a id="start-desktop"></a>

## 第 4 步：启动桌面

源码目录中的 `startup.sh` 已设置 Anland 所需的主要环境变量。以默认 socket 为例，在所选 producer 目录中运行：

```sh
./startup.sh /run/display.sock
```

脚本通常会设置：

```text
ANLAND=1
ANLAND_SOCKET=/run/display.sock
ANLAND_DRM_DEVICE=/dev/dri/renderD128
```

如果使用预构建 RootFS，请优先使用它提供的桌面启动命令。例如 Droidspaces RootFS Desktop Builder 的 KDE 环境可运行：

```sh
startplasma-wayland
```

启动 producer 后：

- chroot 用户打开或切回 Android 的 Anland 应用。
- Droidspaces 用户在运行中容器卡片或容器详情页点击 **Launch Anland**，不要直接点击 Anland 桌面图标。这样才能把该容器的动态 socket 路径传给 Anland。

consumer 和 producer 都连接成功后，桌面画面会自动出现。

> [!IMPORTANT]
>
> 不要用发行版自带、未经 Anland 补丁处理的 `kwin_wayland` 或 Mutter 启动会话。普通 Wayland 合成器即使成功启动，也不会向 Anland 提交画面。

---

<a id="troubleshooting"></a>

## 常见问题

### Anland 只打开设置页，或提示无法连接

chroot 用户先在 Android Root 终端检查默认宿主 socket：

```sh
su -c 'test -S /data/local/tmp/display_daemon.sock'
```

如果不存在，检查 `anland-daemon` 模块是否启用以及设备是否已经重启。如果 socket 存在，确认 Anland 设置中的路径正确、**使用 root 连接**已开启，并且 Root 授权没有被拒绝。

Droidspaces 用户不应检查或填写这个全局默认 socket。请确认：

1. 使用的是带独立 **Anland Display** 开关的 `anland` 分支构建，或该功能合并后的 `main` 版本。
2. 目标容器已经启用 **Anland Display** 并处于运行状态。
3. 容器内存在 `/run/display.sock`。
4. 通过 Droidspaces 的 **Launch Anland** 或 `anland` 按钮打开窗口，而不是点击 Anland 桌面图标。

如果运行中容器没有 Anland 启动按钮，通常表示当前 Droidspaces 版本尚未包含该集成，或该容器的每容器 daemon/socket 没有成功启动。

### 应用已连接，但没有桌面画面

在 Linux 环境中检查：

```sh
test -S /run/display.sock
printf '%s\n' "$ANLAND_SOCKET"
```

确认启动的是经过 Anland 补丁处理的 KWin 或 Mutter，并查看 producer 的终端日志。最常见的原因是 socket 没有映射进 Linux 环境、`ANLAND_SOCKET` 指向错误，或系统更新覆盖了 patched 软件包。

### 桌面启动失败或只能软件渲染

确认 Linux 环境可以访问 `/dev/dri/renderD128`；Adreno 设备还应确认 KGSL 节点和匹配的 Mesa 驱动可用。Droidspaces 用户应重新检查 GPU/硬件访问设置，chroot 用户应检查设备节点挂载与用户权限。

### KDE Plasma 明显卡顿

在 Debian 或 Ubuntu 中，先确认内核启用了 User Namespace。使用 Droidspaces 时，再检查目标版本要求的 `noseccomp` 配置。不要把 SELinux 永久切换为 permissive；应先查看 AVC 日志并修补确切权限。

### 需要调整输入、分辨率、音频或窗口行为

参阅 [Anland 设置使用说明](anland_settings_guide_zh.md)。设置修改后通常会立即保存，但连接、音频和输出分辨率等项目需要在下一次连接时生效。
