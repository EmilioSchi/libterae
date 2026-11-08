<p align="center"><a href="./README.md">English</a> | <a href="./README.zh-CN.md">中文</a></p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark-mode.png" />
    <img src="assets/logo.png" alt="libteræ logo" width="140" />
  </picture>
  <br />
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/name-dark-mode.svg" />
    <img src="assets/name.svg" alt="libteræ" height="48" />
  </picture>
</p>

<p align="center"><sub>MQTT server and client library in C99 · <em>no dynamic memory</em> · desktop, RTOS or bare metal · it never uses more memory than you gave it</sub></p>

<p align="center">
  <a href="include/base_library.h"><img src="https://img.shields.io/badge/version-1.0.5-3C271E" alt="version 1.0.5" /></a>
  <a href="https://en.wikipedia.org/wiki/C99"><img src="https://img.shields.io/badge/language-C99-3C271E" alt="language C99" /></a>
  <a href="https://mqtt.org/"><img src="https://img.shields.io/badge/MQTT-3.1%20%7C%203.1.1%20%7C%205.0-3C271E" alt="MQTT 3.1, 3.1.1 and 5.0" /></a>
  <a href="https://en.wikipedia.org/wiki/MISRA_C"><img src="https://img.shields.io/badge/MISRA%20C%3A2012-adopted-3C271E" alt="MISRA C:2012 adopted" /></a>
  <a href="LICENSING"><img src="https://img.shields.io/badge/license-GPLv3%20%7C%20commercial-3C271E" alt="GPLv3 or commercial license" /></a>
  <br />
  <a href="https://en.wikipedia.org/wiki/C_dynamic_memory_allocation"><img src="https://img.shields.io/badge/memory-static%2C%20no%20malloc-3C271E" alt="static memory, no malloc" /></a>
  <a href="https://en.wikipedia.org/wiki/C_standard_library"><img src="https://img.shields.io/badge/C%20library-not%20needed-3C271E" alt="no C library needed" /></a>
  <a href="https://en.wikipedia.org/wiki/Transport_Layer_Security"><img src="https://img.shields.io/badge/transport-TCP%20%7C%20TLS%20%7C%20WebSocket-3C271E" alt="TCP, TLS and WebSocket" /></a>
  <br />
  <a href="platform"><img src="https://img.shields.io/badge/OS-Linux%20%7C%20macOS%20%7C%20Windows%20%7C%20FreeRTOS%20%7C%20%C2%B5C%2FOS--III-3C271E" alt="Linux, macOS, Windows, FreeRTOS and µC/OS-III" /></a>
  <a href="src/port"><img src="https://img.shields.io/badge/CPU-ARM%20%7C%20RISC--V%20%7C%20x86%20%7C%20MIPS%20%7C%20PowerPC%20%7C%20AVR%20%7C%20%E2%80%A6-3C271E" alt="ARM, RISC-V, x86, MIPS, PowerPC, AVR and others" /></a>
</p>

---

**libteræ** is a library for [MQTT](https://en.wikipedia.org/wiki/MQTT), the lightweight
[publish–subscribe](https://en.wikipedia.org/wiki/Publish%E2%80%93subscribe_pattern) protocol of
[machine-to-machine](https://en.wikipedia.org/wiki/Machine_to_machine) messaging and the
[Internet of Things](https://en.wikipedia.org/wiki/Internet_of_things). It gives both sides of the
protocol, the Server, often called a [broker](https://en.wikipedia.org/wiki/Message_broker), and the
Client, for the three versions in use: [MQTT 5.0](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html)
and [MQTT 3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/mqtt-v3.1.1-errata01-os-complete.html),
standards of [OASIS](https://en.wikipedia.org/wiki/OASIS_%28organization%29), the second also published
as [ISO/IEC 20922](https://www.iso.org/standard/69466.html), and
[MQTT 3.1](https://public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html), from IBM and
Eurotech. The protocol's home is [mqtt.org](https://mqtt.org/).

It is written in [C99](https://en.wikipedia.org/wiki/C99) to the
[MISRA C:2012](https://en.wikipedia.org/wiki/MISRA_C) guidelines, the rules for C in
[embedded systems](https://en.wikipedia.org/wiki/Embedded_system) where safety matters: a MISRA
checker finds nothing in it, which isn't a claim of compliance. It never calls a
[memory allocator](https://en.wikipedia.org/wiki/C_dynamic_memory_allocation) and starts no threads.
All the memory it will use is given to it at the start. The protocol code takes bytes and hands bytes
back, makes no system calls and needs no [C standard library](https://en.wikipedia.org/wiki/C_standard_library),
so the same code runs on a server and on a [microcontroller](https://en.wikipedia.org/wiki/Microcontroller).

Around it, `platform/` gives sockets, clocks and [TLS](https://en.wikipedia.org/wiki/Transport_Layer_Security),
through [Mbed TLS](https://en.wikipedia.org/wiki/Mbed_TLS), with a folder for each system:
[Linux](https://en.wikipedia.org/wiki/Linux) with [epoll](https://en.wikipedia.org/wiki/Epoll),
[macOS](https://en.wikipedia.org/wiki/MacOS) with [kqueue](https://en.wikipedia.org/wiki/Kqueue), any
[POSIX](https://en.wikipedia.org/wiki/POSIX) system, [Windows](https://en.wikipedia.org/wiki/Microsoft_Windows)
with [Winsock](https://en.wikipedia.org/wiki/Winsock), and the
[real-time operating systems](https://en.wikipedia.org/wiki/Real-time_operating_system)
[FreeRTOS](https://en.wikipedia.org/wiki/FreeRTOS) and
[µC/OS-III](https://en.wikipedia.org/wiki/Micro-Controller_Operating_Systems). MQTT travels over
[TCP](https://en.wikipedia.org/wiki/Transmission_Control_Protocol), TLS or
[WebSocket](https://en.wikipedia.org/wiki/WebSocket). It builds for
[ARM](https://en.wikipedia.org/wiki/ARM_architecture_family), [RISC-V](https://en.wikipedia.org/wiki/RISC-V),
[x86](https://en.wikipedia.org/wiki/X86), [MIPS](https://en.wikipedia.org/wiki/MIPS_architecture),
[PowerPC](https://en.wikipedia.org/wiki/PowerPC), [AVR](https://en.wikipedia.org/wiki/AVR_microcontrollers)
and any other processor with a C99 compiler, with an operating system or without one.

## Documentation

| Document | English | 中文 |
| --- | --- | --- |
| **API Reference**: building an MQTT Server or Client with libteræ | [PDF](docs/en/api-reference-en.pdf) | [PDF](docs/zh/api-reference-zh.pdf) |
| **Building libteræ**: the Makefile, from a desktop to a bare board | [PDF](docs/en/building-en.pdf) | [PDF](docs/zh/building-zh.pdf) |
| **Coding Guidelines**: the rules of the C code, and how each one is verified | [PDF](docs/en/coding-guidelines-en.pdf) | [PDF](docs/zh/coding-guidelines-zh.pdf) |
| **Source Code Structure**: how `src/`, `include/` and `platform/` are laid out | [PDF](docs/en/source-structure-en.pdf) | [PDF](docs/zh/source-structure-zh.pdf) |

## License

libteræ is available under the [GNU General Public License](https://en.wikipedia.org/wiki/GNU_General_Public_License),
version 3 or later, in [COPYING](COPYING), or under a commercial license for those who can't use the GPL
([dual licensing](https://en.wikipedia.org/wiki/Multi-licensing)): see [LICENSING](LICENSING). To ask for
the commercial license, write to [emilioschi@gmail.com](mailto:emilioschi@gmail.com). Mbed TLS, which the
TLS build links, keeps its own license: see [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).

© 2026 Emilio Schininà
