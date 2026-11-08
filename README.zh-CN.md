<p align="center"><a href="./README.md">English</a> | <a href="./README.zh-CN.md">中文</a></p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark-mode.png" />
    <img src="assets/logo.png" alt="libteræ 标志" width="140" />
  </picture>
  <br />
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/name-dark-mode.svg" />
    <img src="assets/name.svg" alt="libteræ" height="48" />
  </picture>
</p>

<p align="center"><sub>用 C99 编写的 MQTT 服务器与客户端库 · <em>不用动态内存</em> · 桌面、RTOS 或裸机 · 用的内存从不超过你给它的</sub></p>

<p align="center">
  <a href="include/base_library.h"><img src="https://img.shields.io/badge/%E7%89%88%E6%9C%AC-1.0.5-3C271E" alt="版本 1.0.5" /></a>
  <a href="https://zh.wikipedia.org/wiki/C99"><img src="https://img.shields.io/badge/%E8%AF%AD%E8%A8%80-C99-3C271E" alt="语言 C99" /></a>
  <a href="https://mqtt.org/"><img src="https://img.shields.io/badge/MQTT-3.1%20%7C%203.1.1%20%7C%205.0-3C271E" alt="MQTT 3.1、3.1.1 与 5.0" /></a>
  <a href="https://zh.wikipedia.org/wiki/MISRA_C"><img src="https://img.shields.io/badge/MISRA%20C%3A2012-%E5%B7%B2%E9%87%87%E7%94%A8-3C271E" alt="已采用 MISRA C:2012" /></a>
  <a href="LICENSING"><img src="https://img.shields.io/badge/%E8%AE%B8%E5%8F%AF%E8%AF%81-GPLv3%20%7C%20%E5%95%86%E4%B8%9A-3C271E" alt="GPLv3 或商业许可" /></a>
  <br />
  <a href="https://zh.wikipedia.org/wiki/C%E5%8A%A8%E6%80%81%E5%86%85%E5%AD%98%E5%88%86%E9%85%8D"><img src="https://img.shields.io/badge/%E5%86%85%E5%AD%98-%E9%9D%99%E6%80%81%EF%BC%8C%E4%B8%8D%E7%94%A8%20malloc-3C271E" alt="静态内存，不用 malloc" /></a>
  <a href="https://zh.wikipedia.org/wiki/C%E6%A8%99%E6%BA%96%E5%87%BD%E5%BC%8F%E5%BA%AB"><img src="https://img.shields.io/badge/C%20%E5%BA%93-%E4%B8%8D%E9%9C%80%E8%A6%81-3C271E" alt="不需要 C 库" /></a>
  <a href="https://zh.wikipedia.org/wiki/%E5%82%B3%E8%BC%B8%E5%B1%A4%E5%AE%89%E5%85%A8%E6%80%A7%E5%8D%94%E5%AE%9A"><img src="https://img.shields.io/badge/%E4%BC%A0%E8%BE%93-TCP%20%7C%20TLS%20%7C%20WebSocket-3C271E" alt="TCP、TLS 与 WebSocket" /></a>
  <br />
  <a href="platform"><img src="https://img.shields.io/badge/%E7%B3%BB%E7%BB%9F-Linux%20%7C%20macOS%20%7C%20Windows%20%7C%20FreeRTOS%20%7C%20%C2%B5C%2FOS--III-3C271E" alt="Linux、macOS、Windows、FreeRTOS 与 µC/OS-III" /></a>
  <a href="src/port"><img src="https://img.shields.io/badge/%E5%A4%84%E7%90%86%E5%99%A8-ARM%20%7C%20RISC--V%20%7C%20x86%20%7C%20MIPS%20%7C%20PowerPC%20%7C%20AVR%20%7C%20%E2%80%A6-3C271E" alt="ARM、RISC-V、x86、MIPS、PowerPC、AVR 及其他" /></a>
</p>

---

**libteræ** 是一个 [MQTT](https://zh.wikipedia.org/wiki/MQTT) 库。MQTT 是轻量的[发布/订阅](https://zh.wikipedia.org/wiki/%E5%8F%91%E5%B8%83/%E8%AE%A2%E9%98%85)协议，用于[机器对机器](https://zh.wikipedia.org/wiki/%E6%9C%BA%E5%99%A8%E5%AF%B9%E6%9C%BA%E5%99%A8)通信和[物联网](https://zh.wikipedia.org/wiki/%E7%89%A9%E8%81%94%E7%BD%91)。本库提供协议的两端：服务器（常被称为[消息代理](https://zh.wikipedia.org/wiki/%E6%B6%88%E6%81%AF%E4%BB%A3%E7%90%86)，broker）和客户端，支持现行的三个版本：[MQTT 5.0](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html) 和 [MQTT 3.1.1](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/errata01/os/mqtt-v3.1.1-errata01-os-complete.html) 是 [OASIS](https://zh.wikipedia.org/wiki/%E7%BB%93%E6%9E%84%E5%8C%96%E4%BF%A1%E6%81%AF%E6%A0%87%E5%87%86%E4%BF%83%E8%BF%9B%E7%BB%84%E7%BB%87) 的标准，后者也以 [ISO/IEC 20922](https://www.iso.org/standard/69466.html) 发布；[MQTT 3.1](https://public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html) 来自 IBM 和 Eurotech。协议的主页是 [mqtt.org](https://mqtt.org/)。

它用 [C99](https://zh.wikipedia.org/wiki/C99) 编写，遵循 [MISRA C:2012](https://zh.wikipedia.org/wiki/MISRA_C)，即面向注重安全的[嵌入式系统](https://zh.wikipedia.org/wiki/%E5%B5%8C%E5%85%A5%E5%BC%8F%E7%B3%BB%E7%BB%9F)的 C 语言规范：MISRA 检查工具在其中找不到任何问题，但这并不是合规声明。它从不调用[内存分配器](https://zh.wikipedia.org/wiki/C%E5%8A%A8%E6%80%81%E5%86%85%E5%AD%98%E5%88%86%E9%85%8D)，也不创建线程，所需的全部内存都在启动时交给它。协议代码接收字节、交回字节，不做系统调用，也不需要 [C 标准库](https://zh.wikipedia.org/wiki/C%E6%A8%99%E6%BA%96%E5%87%BD%E5%BC%8F%E5%BA%AB)，因此同一份代码既能运行在服务器上，也能运行在[微控制器](https://zh.wikipedia.org/wiki/%E5%BE%AE%E6%8E%A7%E5%88%B6%E5%99%A8)上。

在它之外，`platform/` 提供套接字、时钟和 [TLS](https://zh.wikipedia.org/wiki/%E5%82%B3%E8%BC%B8%E5%B1%A4%E5%AE%89%E5%85%A8%E6%80%A7%E5%8D%94%E5%AE%9A)（通过 [Mbed TLS](https://zh.wikipedia.org/wiki/Mbed_TLS)），每个系统一个文件夹：使用 [epoll](https://zh.wikipedia.org/wiki/Epoll) 的 [Linux](https://zh.wikipedia.org/wiki/Linux)、使用 [kqueue](https://zh.wikipedia.org/wiki/Kqueue) 的 [macOS](https://zh.wikipedia.org/wiki/MacOS)、任何 [POSIX](https://zh.wikipedia.org/wiki/POSIX) 系统、使用 [Winsock](https://zh.wikipedia.org/wiki/Winsock) 的 [Windows](https://zh.wikipedia.org/wiki/Microsoft_Windows)，以及[实时操作系统](https://zh.wikipedia.org/wiki/%E5%AE%9E%E6%97%B6%E6%93%8D%E4%BD%9C%E7%B3%BB%E7%BB%9F) [FreeRTOS](https://zh.wikipedia.org/wiki/FreeRTOS) 和 [µC/OS-III](https://en.wikipedia.org/wiki/Micro-Controller_Operating_Systems)。MQTT 可以经由 [TCP](https://zh.wikipedia.org/wiki/%E4%BC%A0%E8%BE%93%E6%8E%A7%E5%88%B6%E5%8D%8F%E8%AE%AE)、TLS 或 [WebSocket](https://zh.wikipedia.org/wiki/WebSocket) 传输。它可以为 [ARM](https://zh.wikipedia.org/wiki/ARM%E6%9E%B6%E6%A7%8B)、[RISC-V](https://zh.wikipedia.org/wiki/RISC-V)、[x86](https://zh.wikipedia.org/wiki/X86)、[MIPS](https://zh.wikipedia.org/wiki/MIPS%E6%9E%B6%E6%A7%8B)、[PowerPC](https://zh.wikipedia.org/wiki/PowerPC)、[AVR](https://zh.wikipedia.org/wiki/AVR) 以及任何有 C99 编译器的处理器构建，有没有操作系统都可以。

## 文档

| 文档 | English | 中文 |
| --- | --- | --- |
| **API 参考**：用 libteræ 构建 MQTT 服务器或客户端 | [PDF](docs/en/api-reference-en.pdf) | [PDF](docs/zh/api-reference-zh.pdf) |
| **构建 libteræ**：Makefile，从桌面到裸板 | [PDF](docs/en/building-en.pdf) | [PDF](docs/zh/building-zh.pdf) |
| **编码规范**：C 代码所遵循的规则，以及每一条如何被验证 | [PDF](docs/en/coding-guidelines-en.pdf) | [PDF](docs/zh/coding-guidelines-zh.pdf) |
| **源码结构**：`src/`、`include/` 和 `platform/` 怎样分布 | [PDF](docs/en/source-structure-en.pdf) | [PDF](docs/zh/source-structure-zh.pdf) |

## 许可证

libteræ 可以在 [GNU 通用公共许可证](https://zh.wikipedia.org/wiki/GNU%E9%80%9A%E7%94%A8%E5%85%AC%E5%85%B1%E8%AE%B8%E5%8F%AF%E8%AF%81)第 3 版或（由你选择）任何更新的版本下使用，全文见 [COPYING](COPYING)；无法使用 GPL 的用户可以取得商业许可（[多重授权](https://zh.wikipedia.org/wiki/%E5%A4%9A%E9%87%8D%E6%8E%88%E6%AC%8A)），见 [LICENSING](LICENSING)。如需商业许可，请发邮件至 [emilioschi@gmail.com](mailto:emilioschi@gmail.com)。TLS 构建所链接的 Mbed TLS 保留其自身的许可证，见 [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)。

© 2026 Emilio Schininà
