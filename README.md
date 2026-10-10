# FujinNX 🌀

Custom fan curve for modded Nintendo Switch / Switch Lite — a background sysmodule plus a Tesla overlay with live temperatures, curve presets, and a thermal failsafe. Built for consoles where the stock fan curve ramps up too late.

> **⚠️ USE AT YOUR OWN RISK**
>
> FujinNX takes over your console's fan control and disables the OS's own thermal fan management. It is provided **as-is, without any warranty** (see [LICENSE](LICENSE)).
>
> - Running custom firmware and sysmodules can **brick your console** or get your Nintendo account **banned**.
> - A wrong fan curve can make your console run **hotter than stock**, potentially damaging hardware.
> - This has only been tested on one modded Switch Lite. Your hardware may behave differently.
> - Do **not** run FujinNX alongside other fan-control homebrew (e.g. NX-FanControl) — two writers fighting over the fan can crash or reboot the console.
>
> The authors accept **no responsibility** for any damage to your console, loss of data, or account bans. If you are not comfortable with that, do not install this.

## Features

- **Custom temperature → fan curve** with linear interpolation between points
- Reads both **SoC** and **skin** temperature; can follow either or the hotter of the two
- **Asymmetric ramping** — spins up instantly, slows down gradually (no fan yo-yo)
- **Failsafe**: fan forced to 100% if sensors can't be read or temperature exceeds a limit (default 75 °C)
- **Tesla overlay** with live temps, fan %, curve presets (Quiet / Balanced / Cool / Max) and settings
- Config hot-reload: edit `config.ini` (by hand or via the overlay) and it applies within ~2 seconds

## Requirements

- Nintendo Switch / Switch Lite with [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) CFW
- [Tesla](https://github.com/WerWolv/Tesla-Menu) (nx-ovlloader + Tesla Menu) for the overlay

## Install

1. Copy the contents of `dist/sd/` to the root of your SD card (merge folders).
2. Reboot the console.
3. Open the Tesla menu (`L + D-pad Down + R-stick click`) → **FujinNX**.

The sysmodule starts automatically at boot (`boot2.flag`).

## Configuration

`sd:/config/fujinnx/config.ini` — created on first boot:

```ini
[settings]
source=max            ; soc | skin | max (hotter of the two)
min_duty=20           ; fan never goes below this percent
ramp_down=3           ; max percent the fan may slow per second
failsafe_temp=75      ; at or above this the fan is forced to 100%
[curve]               ; temperature_C=fan_percent
30=20
35=30
40=60
43=80
45=100
```

Live status is written to `sd:/config/fujinnx/status.txt`.

## Building

Requires [devkitPro](https://devkitpro.org/) with the `switch-dev` package group.

- **Windows:** run `build.ps1` (handles paths with spaces via a temporary `subst` drive)
- **Linux/macOS:** `make` inside `sysmodule/` and `overlay/`

Clone with submodules:

```
git clone --recurse-submodules https://github.com/krisnapradyana/FujinNX.git
```

## How it works

| Component | What it does |
|---|---|
| `sysmodule/` | Runs at boot (title ID `420000000000F001`). Polls `ts`/`tc` temperature services every second, disables OS fan control (`tc`), and drives the fan via the `fan` service. |
| `overlay/` | Tesla overlay. Shows live readings, writes presets/settings to `config.ini`, which the sysmodule hot-reloads. |

## Credits & License

- [libtesla](https://github.com/WerWolv/libtesla) by WerWolv (GPLv2), used as a submodule
- [libnx](https://github.com/switchbrew/libnx) & [devkitPro](https://devkitpro.org/)

FujinNX is licensed under the **GPL-2.0** — see [LICENSE](LICENSE).
