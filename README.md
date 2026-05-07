# trigger-shot

A product of J2i.net, LLC 2026. All Rights Reserved.

A Win32 64-bit command-line utility for automated time-lapse photography.  
It enumerates all WIA-compatible cameras connected to the system, creates a
dedicated output sub-folder for each camera, and captures a photograph from
every camera at a configurable interval.

## Requirements

* Windows 10 / 11 (x64)
* Visual Studio 2022 (v143 toolset)
* Windows SDK 10.0
* One or more WIA-compatible cameras attached to the system

## Building

Open `TriggerShot.sln` in Visual Studio 2022 and build the **Release|x64**
(or **Debug|x64**) configuration, or use MSBuild from a Developer Command
Prompt:

```
msbuild TriggerShot.sln /p:Configuration=Release /p:Platform=x64
```

The compiled binary is placed in `x64\Release\TriggerShot.exe` (or
`x64\Debug\TriggerShot.exe`).

## Usage

```
TriggerShot <interval_seconds> <output_folder> [count]
```

| Argument          | Description                                                       |
|-------------------|-------------------------------------------------------------------|
| `interval_seconds`| Time between capture rounds, in seconds (must be ≥ 1)            |
| `output_folder`   | Root folder for captured images; created if it does not exist     |
| `count`           | *(Optional)* Number of capture rounds; omit to run until CTRL-C  |

### Examples

Capture every 30 seconds into `C:\TimeLapse`, running until CTRL-C:
```
TriggerShot 30 C:\TimeLapse
```

Capture every 10 seconds, 120 rounds (20 minutes total):
```
TriggerShot 10 C:\TimeLapse 120
```

## Output folder layout

```
<output_folder>\
    <CameraName>\
        <CameraName>_YYYYMMDD_HHMMSS_000001.jpg
        <CameraName>_YYYYMMDD_HHMMSS_000002.jpg
        ...
```

Each image file name is composed of:
* **Camera name** — sanitised to be a valid Windows file-name
* **Date/time** — `YYYYMMDD_HHMMSS` of the capture moment
* **Sequence number** — six-digit zero-padded counter that increments with each round

## Stopping

Press **CTRL-C** to stop the program gracefully after the current round
finishes.
