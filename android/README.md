# iptsd-runner — Android IPTS Touch Service

A native C/C++ service that manages [iptsd](https://github.com/linux-surface/iptsd)
on Android (Android-x86 / BlissOS) for Microsoft Surface devices using Intel
Precise Touch (IPTS) technology.

## Why

The upstream `iptsd` daemon handles a single hidraw device passed on the command
line. On Android there's no systemd or udev to discover devices, handle
hotplug, or recover from suspend/resume. `iptsd-runner` fills that gap:

- **Auto-detection** — scans `/dev/hidraw*` and identifies IPTS devices by
  parsing HID descriptors (no scripts, no hardcoded paths).
- **Multi-device** — spawns one `iptsd` instance per device (e.g. touchscreen +
  touchpad on Surface Laptop Studio).
- **Hotplug** — watches `/dev/` via `inotify` for device add/remove events.
- **Suspend/resume** — detects resume via `CLOCK_BOOTTIME` drift and optionally
  reloads the kernel module to work around driver bugs.
- **Process supervision** — monitors children via `signalfd`/`SIGCHLD`, reaps
  dead instances, and rescans.
- **Non-IPTS bail-out** — exits cleanly on hardware without IPTS (no wasted
  resources on non-Surface devices).

## Architecture

```
Android init
  └── iptsd-runner (this service)
        │
        ├── Startup
        │   ├── Check /proc/modules for ipts / ithc
        │   ├── No modules + no devices → exit(0)
        │   └── Scan /dev/hidraw* → probe HID descriptors
        │
        ├── Per IPTS device found
        │   └── fork + exec iptsd /dev/hidrawN
        │
        └── epoll main loop
            ├── inotify /dev/       → hidraw add/remove → debounced rescan
            ├── signalfd SIGCHLD    → reap dead children → rescan
            ├── timerfd MONOTONIC   → debounce timer for rescans
            └── timerfd BOOTTIME    → resume detection
                                      └── if reload enabled: kill all →
                                          reload module(s) → rescan
```

## Source Files

| File | Description |
|------|-------------|
| `ipts_detector.h` / `.cpp` | Pure-C HID descriptor parser. Identifies IPTS devices by checking for the modesetting feature report (Usage Page `0xFF00`, Usage `0xC8`) and touch data input reports (Scan Time + Gesture Data). Determines touchscreen vs touchpad. |
| `device_manager.h` / `.cpp` | Core orchestrator. Manages the instance table, scanning, `fork`/`exec`, child reaping, resume detection, and the `epoll` main loop. |
| `main.cpp` | Entry point. Signal handlers, init, early bail-out on non-IPTS hardware, main loop. |
| `Android.bp` | Soong build definition. |
| `iptsd-runner.rc` | Android init service + property triggers for module reload. |
| `sepolicy/` | SELinux policy (see below). |

## Supported Drivers

| Module | Hardware | Type |
|--------|----------|------|
| `ipts` | Surface Pro 4 – Surface Pro 7 | MEI-based |
| `ithc` | Surface Pro 7+ and newer | PCI-based (Intel Touch Host Controller) |

Both drivers create hidraw devices with the same IPTS HID protocol. The
detector works identically for both.

## Building

The module is built as part of the Android tree via Soong:

```bash
mmm external/iptsd/android
```

This produces `/vendor/bin/iptsd-runner` and installs the `.rc` file.

**Prerequisites**: The main `iptsd` binary and its presets must also be built
and installed (the `Android.bp` declares `required: ["iptsd", "iptsd-presets"]`).

## Integration

### 1. Device tree init script

In your `init.<target>.rc`:

```rc
on property:sys.boot_completed=1
    start iptsd-runner
```

### 2. ueventd permissions

Ensure hidraw devices are accessible. Example in `ueventd.rc` or
`ueventd.<target>.rc`:

```
/dev/hidraw*    0660    system    system
```

### 3. SELinux policy

Add to your `BoardConfig.mk`:

```makefile
BOARD_SEPOLICY_DIRS += external/iptsd/android/sepolicy
```

The policy provides:

| File | Contents |
|------|----------|
| `iptsd.te` | Domain `iptsd`, type `hidraw_device`, allow rules for hidraw/uinput/config access, process management, property access, `/proc/modules` read |
| `file_contexts` | Labels for `/vendor/bin/iptsd*` → `iptsd_exec`, `/dev/hidraw*` → `hidraw_device` |
| `property_contexts` | Labels for `vendor.iptsd.*` and `persist.vendor.iptsd.*` → `iptsd_prop` |

## Properties

All properties use the `vendor.` prefix for Treble compliance.

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `persist.vendor.iptsd.reload_on_resume` | `bool` | `0` | Enable (`1`) or disable (`0`) automatic kernel module reload on resume. See [Resume Workaround](#resume-workaround). |
| `vendor.iptsd.reload_driver` | `string` | — | Set by the runner to trigger module reload via init. Values: `ipts`, `ithc`, or `ipts,ithc`. Automatically reset to `0` after processing. **Do not set manually.** |

### Toggling at runtime

Properties are re-read on each resume event, so changes take effect without
restarting the service:

```bash
# Enable module reload workaround
setprop persist.vendor.iptsd.reload_on_resume 1

# Disable it
setprop persist.vendor.iptsd.reload_on_resume 0
```

## Resume Workaround

### The problem

The `ipts` kernel driver can fail to reinitialize after suspend/resume:

```
ipts 0000:00:16.4: Failed to set memory window: 1
ipts 0000:00:16.4: Failed to switch modes: 1
```

When this happens the hidraw device **stays** (no hotplug event), so the
runner can't detect the failure through normal monitoring. Touch stops
working silently.

One solution that might work is to run `rmmod ipts && modprobe ipts`.

### The solution

When `persist.vendor.iptsd.reload_on_resume` is set to `1`:

1. **Detect resume** — a periodic `CLOCK_BOOTTIME` timer fires every 2 seconds.
   `CLOCK_BOOTTIME` includes suspend time while `CLOCK_MONOTONIC` does not.
   When the difference between them increases by ≥1 second, a suspend/resume
   cycle has occurred.
2. **Kill all iptsd instances** — `SIGTERM` → wait → `SIGKILL` stragglers.
3. **Reload module(s)** — sets `vendor.iptsd.reload_driver` property to the
   name(s) of loaded modules. The `.rc` property trigger runs
   `modprobe -r <module> && modprobe <module>` via init (in the
   `vendor_modprobe` SELinux domain).
4. **Wait** — 3 seconds for the driver to create new hidraw devices.
5. **Rescan** — finds the new devices and spawns fresh iptsd instances.

When disabled (`0`), the runner still detects resume but only kills stale
iptsd instances and rescans — no module reload.

## Non-IPTS Hardware

On startup, if **no IPTS modules are loaded** (`ipts`/`ithc` not in
`/proc/modules`) **and no IPTS devices are found**, the service logs:

```
iptsd-runner: No IPTS modules loaded and no devices found —
              this hardware does not appear to have IPTS touch. Exiting.
```

…and exits with code 0. No memory wasted, no background polling.

If modules are loaded but devices haven't appeared yet (slow init), the
runner waits in the epoll loop for `inotify` events.

## Service Lifecycle

```
start iptsd-runner    → init starts the service
                      → scans, spawns iptsd per device
                      → enters main loop

stop iptsd-runner     → init sends SIGTERM
                      → runner kills all iptsd children (SIGTERM → SIGKILL)
                      → exits cleanly
                      → hidraw devices freed for iptsd-calibrate etc.
```

## License

GPL-2.0-or-later (same as iptsd).
