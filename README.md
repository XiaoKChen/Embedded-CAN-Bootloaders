# <Project Name>

<One-paragraph description: what the firmware does, target MCU/board, key
peripherals it drives, and how it communicates with the rest of the system.>

---

## Hardware / Peripherals

<Describe wiring location — usually `src/main.{c,cpp}` — and tabulate the
actuators, sensors, and bus configuration.>

### Actuators

| Device | Driver | Bus / Channel | Notes |
| ------ | ------ | ------------- | ----- |
|        |        |               |       |

### Sensors

| Device | Driver | Bus / Channel | Notes |
| ------ | ------ | ------------- | ----- |
|        |        |               |       |

### Bus configuration

| Bus | Configuration |
| --- | ------------- |
|     |               |

---

## Communication

<Describe the transport (CAN, UART, Modbus, etc.), the message schema
(Protocol Buffers, plain bytes, …), and the request/response endpoints
the board exposes.>

---

## Build & Flash

<Build system: PlatformIO, CMake, Make, etc. Include the exact commands.>

```sh
# Build
<command>

# Flash
<command>

# Monitor
<command>
```

Language standards: **C `<standard>`** and **C++ `<standard>`**. CPU flags:
`<-mcpu=... -mfpu=... -mfloat-abi=... -mthumb>`.

---

## Repo Layout

```
.
├── src/         <one-line description>
├── include/     <one-line description>
├── lib/         <one-line description>
├── test/        <one-line description>
└── <build-config-file>
```

---

## Dependencies

<List runtime libraries, where they are declared (lib_deps, submodules,
vendor/), and how they are pulled in.>
