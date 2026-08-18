# Hacktor Watch 2.0 Firmware

Zephyr application for the `Hacktor Watch 2.0`: a round SPI LCD, a capacitive
touch panel, an analog watch face and an app menu.

Vendored from [dantudose/Hacktor_Basic](https://github.com/dantudose/Hacktor_Basic) (GPL-3.0), with fixes for
Zephyr 4.4 — see the commit history for what changed and why.

What it does:

- routes the Zephyr console and shell to the ESP32-S3 native USB serial/JTAG port
- drives a `GC9A01` 240x240 round LCD over `SPI2` and a `CST816T` touch panel on `I2C`
- draws an analog watch face with battery state and a step count
- opens an app menu from a button on the face
- sleeps the display after 30 s and wakes it on touch or a wrist gesture
- logs battery discharge to RAM so power work can be measured on the hardware

## Hardware

Target board:

- `esp32s3_devkitc/esp32s3/procpu`

Connected peripherals:

- LCD controller: `GC9A01`
- Touch controller: `CST816T` using Zephyr's `hynitron,cst8xx` driver path
- IMU: `LSM6DSL`, driven by direct I2C register access rather than a Zephyr driver
- Fuel gauge: `MAX17048`

## Pin Mapping

LCD, on `SPI2`. The schematic labels these nets `SPI3_*`, but they land on the
IO_MUX pins of the ESP32-S3 peripheral Zephyr calls `spi2`, and `&spi3` is
disabled in the overlay:

- `SCK` -> `GPIO12`
- `MOSI` -> `GPIO11`
- `MISO` -> `GPIO13`
- `LCD_CS` -> `GPIO4`
- `LCD_DC` -> `GPIO5`
- `LCD_RST` -> `GPIO3`
- `LCD_BL` -> `GPIO7`
- `LCD power enable` -> `GPIO18`

I2C bus, shared by the touch panel, IMU, fuel gauge and haptic driver:

- `I2C_SDA` -> `GPIO8`
- `I2C_SCL` -> `GPIO9`

Interrupts and resets:

- `TP_INT` -> `GPIO1`
- `TP_RST` -> `GPIO2`
- `IMU_INT1` -> `GPIO17`
- `IMU_INT2` -> `GPIO14`
- charger status -> `GPIO21`

I2C addresses: touch `0x15`, fuel gauge `0x36`, haptic `0x5a`, IMU `0x6a`.

These connections are described in [app.overlay](app.overlay).

## What The App Does

The watch face is drawn from LVGL primitives: sixty tick dots, numerals at 12, 3
and 6, three hands, and a red dot marking the current hour, minute and second.

There is no RTC on this board. The clock is seeded from `__DATE__` and `__TIME__`
at build time and counts up from there, so it restarts on every boot and is only
ever roughly right.

Around the face:

- **Battery** — a percentage, an icon that fills proportionally, and a bolt while
  charging. Read from the `MAX17048` every 10 s. Red below 15%, blue above 90%.
- **Steps** — read every second from the `LSM6DSL` hardware pedometer, which
  counts on its own and keeps counting while the display is asleep.
- **Menu** — the hamburger button on the left opens a second screen listing the
  apps. Its rows are positioned by hand and scrolled from a rail down the side,
  because LVGL's own scrollable container swallows presses on this touch panel.

The display is rotated 180 degrees in devicetree so the UI matches the physical
mounting orientation.

### Power Management

The display sleeps 30 s after the last interaction. Sleeping means the backlight
goes off and the panel gets a `DISPOFF`; the `3V3_2` rail deliberately stays up,
because it is shared with the touch controller and dropping it wedges the I2C bus
for every device on it — including the IMU, which sits on the other rail.

Two things wake the screen:

- a touch, as `INPUT_BTN_TOUCH`
- a wrist gesture, from the `LSM6DSL` wake-up interrupt on `INT1`/`INT2`, limited
  to one wake every 2 s

Plugging in the charger does not wake it.

While the display sleeps the main thread blocks on a semaphore and polls nothing.
Nothing is lost by sleeping: the pedometer counts in hardware, and the fuel gauge
sits on the raw battery rail.

Measured idle draw is roughly 12-14 mA, which is about 14 h on the 200 mAh cell.
`CONFIG_PM` is not enabled, so the SoC never enters light sleep, and the display
controller is only blanked rather than put to sleep.

### Threads

- `main` — the display power state machine and sensor polling
- `lvgl_ui` — runs `lv_timer_handler`, parked while the display sleeps
- `batlog` — samples the fuel gauge into the discharge log

Anything touching LVGL takes `lvgl_lock()` first, and **nothing reached from an
LVGL callback may print**: the USB console blocks when no host is draining it, and
these callbacks run while holding the LVGL lock, so one stray `printk` freezes the
interface.

## Project Structure

- [CMakeLists.txt](CMakeLists.txt): Zephyr app definition and source list
- [prj.conf](prj.conf): Zephyr Kconfig options for console, shell, LVGL, display, input
- [app.overlay](app.overlay): board-specific devicetree overlay with LCD and touch wiring
- [src/main.c](src/main.c): minimal entry point
- [src/panel.c](src/panel.c): watch face, power management, sensors, and shell commands
- [src/menu.c](src/menu.c): the app menu screen and its scroll rail
- [src/battery_log.c](src/battery_log.c): fuel gauge sampler behind the `batlog` shell command
- [build.sh](build.sh): local build/flash helper script

## Zephyr Setup

Do this once, on a fresh machine. Most of the time is downloads.

Zephyr is a workspace, not a library you install into a project. It lives in its
own directory, separate from this repository, and this application is pointed at
it. These are the versions the firmware is known to build against:

| | version |
|---|---|
| Zephyr | 4.4.2 |
| west | 1.5.0 |
| Zephyr SDK | 1.0.1 |
| Python | 3.13 |

Only step 1 differs between platforms. Steps 2 to 6 are the same everywhere,
apart from how the virtual environment is activated.

### 1. Host tools

#### macOS

```bash
brew install cmake ninja gperf python3 ccache qemu dtc libmagic wget
```

#### Linux (Debian / Ubuntu)

```bash
sudo apt install --no-install-recommends git cmake ninja-build gperf ccache \
  dfu-util device-tree-compiler wget python3-dev python3-venv python3-tk \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1
```

Zephyr needs CMake 3.20 or newer. Check with `cmake --version`; on older
distributions the packaged version is too old and you need the
[Kitware APT repository](https://apt.kitware.com/).

Serial ports are root-owned by default, so flashing fails with a permission
error until your user is in the `dialout` group:

```bash
sudo usermod -aG dialout $USER
```

Log out and back in for it to take effect.

#### Windows

Two routes, and the choice matters more than it looks.

**Native Windows** is the simpler one for this board, because flashing needs
direct access to a USB serial device:

```powershell
winget install Kitware.CMake Ninja-build.Ninja oss-winget.gperf oss-winget.dtc `
  oss-winget.wget 7zip.7zip Git.Git Python.Python.3.12
```

Then enable long path support, which Zephyr's deep build trees need:

```powershell
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" `
  -Name "LongPathsEnabled" -Value 1 -PropertyType DWORD -Force
```

**WSL2** works for building and gives you the Linux instructions above, but it
does *not* see USB devices by default. Flashing from inside WSL2 requires
[usbipd-win](https://github.com/dorssel/usbipd-win) to attach the board to the
Linux side, which is extra moving parts on every session. If you go this route,
consider building in WSL2 and flashing from Windows.

### 2. Workspace and virtual environment

macOS and Linux:

```bash
python3 -m venv ~/zephyrproject/.venv
source ~/zephyrproject/.venv/bin/activate
pip install west
```

Windows PowerShell:

```powershell
python -m venv $HOME\zephyrproject\.venv
$HOME\zephyrproject\.venv\Scripts\Activate.ps1
pip install west
```

If PowerShell refuses to run the activation script, allow local scripts once:

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

Keep the virtual environment active for the rest of the setup, and remember that
every later session needs it too — `west` and `esptool` are not on the path
without it.

### 3. Zephyr and its modules

```bash
west init ~/zephyrproject
cd ~/zephyrproject
west update
west zephyr-export
west packages pip --install
```

`west update` clones every module in the manifest. It takes several minutes.

### 4. Espressif binary blobs

**Do not skip this step.** The ESP32-S3 build links against closed-source
Espressif libraries that the manifest does not carry. Without them the build
fails at link time with unresolved symbols and no explanation of the cause.

```bash
west blobs fetch hal_espressif
```

### 5. Toolchain

The full SDK is large and this board needs exactly one target:

```bash
west sdk install --toolchains xtensa-espressif_esp32s3_zephyr-elf
```

### 6. Confirm the environment works

Build an unrelated sample before touching this project, so that a later failure
is clearly the application's fault rather than the environment's:

```bash
cd ~/zephyrproject
west build -b esp32s3_devkitc/esp32s3/procpu zephyr/samples/hello_world --pristine
```

A working setup ends with a memory usage table.

## Building

`build.sh` has to find Zephyr. It looks for `../zephyr` next to this directory,
which only works if the workspace happens to sit beside the repository — it
normally does not. Set `ZEPHYR_BASE` instead, and put the virtual environment on
the path so `esptool` is found at the image-generation step:

```bash
export ZEPHYR_BASE=~/zephyrproject/zephyr
export PATH="$HOME/zephyrproject/.venv/bin:$PATH"
```

Put both lines in your shell profile and every command below works as written.

Build:

```bash
./build.sh
```

Clean only:

```bash
./build.sh --clean
```

Pristine rebuild:

```bash
./build.sh --pristine
```

Override the default board if needed:

```bash
BOARD=esp32s3_devkitc/esp32s3/procpu ./build.sh
```

The helper script is a convenience, not a requirement. The equivalent plain
`west` invocation, run from anywhere inside the workspace:

```bash
cd ~/zephyrproject
west build -b esp32s3_devkitc/esp32s3/procpu /path/to/Hacktor-Watch-2.0/Firmware
```

## Flashing

Flash the board:

```bash
./build.sh --flash --port /dev/cu.usbmodemXXXX
```

Optional erase before flashing:

```bash
./build.sh --flash --erase --port /dev/cu.usbmodemXXXX
```

After flashing, the script opens a serial terminal at `115200`.

The port is named differently on each platform:

| | port |
|---|---|
| macOS | `/dev/cu.usbmodemXXXX` |
| Linux | `/dev/ttyACM0` |
| Windows | `COM4` |

List candidates with `ls /dev/cu.*` on macOS, `ls /dev/ttyACM*` on Linux, or
`Get-CimInstance Win32_SerialPort` in PowerShell.

The board exposes the ESP32-S3 native USB serial/JTAG peripheral, so the port
only appears while the firmware is running. If it vanishes, the board has reset
or the battery is flat.

## USB Console And Shell

The project routes both console and shell to the ESP32-S3 USB serial/JTAG peripheral.

Once connected, the Zephyr shell is available together with two project commands.

`app` prints display geometry and power state, IMU status, battery percentage,
charging state, step count and the current clock:

```text
app
```

`batlog` drives the battery discharge log:

```text
batlog stat        summary: idle draw, screen-on draw, projected runtime
batlog dump [n]    the sample table
batlog csv         the same, for a spreadsheet
batlog now         instant charge, cell voltage and discharge rate
batlog period <s>  show or set the sample period (10..3600 s)
batlog clear       discard the log and start a fresh run
```

The log lives in RAM and is lost on reset, which matters because a measurement
cannot be read while it is running: USB powers the board through `VBUS` and
disconnects the cell, so plugging in to read the console ends the discharge. Run
the test, plug in afterwards, then read. Do not let the battery reach empty —
the brownout takes the log with it.

## Notes

- The LCD panel is configured through Zephyr's `galaxycore,gc9x01x` driver.
- The touch controller is described as `hynitron,cst816s` in devicetree because that is the supported Zephyr driver path used by this hardware setup.
- The `LSM6DSL` is described in devicetree as a plain `i2c-device`. Wake-on-wrist
  and the pedometer are configured by writing its registers directly, because the
  Zephyr sensor driver does not expose the embedded functions this needs.
- The `MAX17048` registers are also read directly in [src/battery_log.c](src/battery_log.c).
  The Zephyr driver truncates the state of charge to whole percent, and one
  percent of this cell is 2 mAh — too coarse to compare one power change against
  the next.
