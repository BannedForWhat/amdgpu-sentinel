# amdgpu-sentinel

[![Platform](https://img.shields.io/badge/Platform-Arch_Linux-blue.svg?logo=arch-linux)](https://archlinux.org/)
[![Kernel](https://img.shields.io/badge/Kernel-6.x-green.svg?logo=linux)](https://kernel.org/)
[![GPU](https://img.shields.io/badge/GPU-RDNA_2_(Navi_22)-red.svg?logo=amd)](https://www.amd.com/)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17_Zero--Alloc-orange.svg?logo=c%2B%2B)](https://en.cppreference.com/)
[![Sandbox](https://img.shields.io/badge/Sandbox-systemd_cgroups_v2-black.svg?logo=systemd)](https://systemd.io/)

Низкоуровневый безаллокационный C++17 сервис мониторинга графических ускорителей AMD на микроархитектуре RDNA 2 (Radeon RX 6700 XT / Navi 22 / `gfx1031`). 

Демон выполняет непрерывную телеметрию пулов видеопамяти (VRAM), отслеживает вытеснение страниц через менеджер ядра DRM TTM в оперативную память хоста (GTT), опрашивает процент занятости вычислительных блоков GPU и температуру через виртуальную файловую систему `sysfs` (`kernfs`). Спроектирован для контроля стабильности локальной AI-инфраструктуры (`llama.cpp` / ROCm) и превентивного обнаружения падения скорости инференса при переполнении памяти.

---

## Архитектурные инварианты (Zero-Allocation Engine)

В отличие от стандартных мониторов (`rocm-smi`, `nvtop`), линкующих высокоуровневые рантаймы и циклически открывающих файлы через `open()`/`close()`, `amdgpu-sentinel` спроектирован по принципам детерминированного системного программирования:

1. **Persistent FDs & VFS Lockless `pread()`:** Все файловые дескрипторы узлов ядра `/sys/class/drm/card*/device/` открываются однократно на этапе инициализации. Чтение выполняется системным вызовом `pread(fd, buf, size, 0)` со смещением 0. Это обращается напрямую к `fdget()` ядра без захвата блокировки `f_pos_lock`, исключая накладные расходы VFS и состояния гонки (TOCTOU).
2. **Аппаратный реактор `epoll` + `timerfd`:** Опрос с шагом 1.0 с управляется монотонным аппаратным таймером `timerfd_create(CLOCK_MONOTONIC)` через демультиплексор `epoll_pwait2` (ядро Linux 6.x). Суммарный дрейф таймера (cumulative drift) равен **0 мс**.
3. **Синхронная обработка сигналов UNIX:** Сигналы `SIGINT` и `SIGTERM` маскируются в потоке и принимаются через дескриптор `signalfd4`. Это исключает асинхронные обработчики сигналов, предотвращая гонки памяти и вызовы async-signal-unsafe функций.
4. **Отказоустойчивость SMU/DPM (Navi 22):** При переходах состояний энергосбережения шины PCIe (GFXOFF, BACO, Runtime D3cold) драйвер `amdgpu` может возвращать `-EAGAIN`, `-EBUSY` или `-EINVAL`. Демон удерживает последнее валидное состояние и выставляет флаг деградации без генерации исключений и паники процесса.
5. **Ultra-Low Memory Footprint (~420 КБ RSS):** Полное отключение исключений C++ (`-fno-exceptions`) и RTTI (`-fno-rtti`), секционирование кода (`-ffunction-sections`, `-fdata-sections`), стриппинг (`-s`) и LTO-оптимизация компилятора GCC обеспечивают потребление памяти в **~420 Килобайт** физической ОЗУ.

---

## Архитектура системы

```text
 +-------------------------------------------------------------------------+
 |                            Linux Kernel Space                           |
 |                                                                         |
 |   +-----------------------+     +-------------------+     +---------+   |
 |   |  amdgpu (TTM Manager) |     |  SMU11 / DPM Core |     |  hwmon  |   |
 |   +-----------+-----------+     +---------+---------+     +----+----+   |
 |               |                           |                    |        |
 |               v                           v                    v        |
 |   /sys/class/drm/cardX/device/   .../gpu_busy_percent      .../temp1_   |
 |   mem_info_{vram,gtt}_*                                    input        |
 +---------------+---------------------------+--------------------+--------+
                 |                           |                    |
                 | pread(fd, buf, 64, 0)     | (Persistent FDs)   |
                 v                           v                    v
 +-------------------------------------------------------------------------+
 |                             amdgpu-sentinel                             |
 |                                                                         |
 |  [ timerfd (1.0s) ] ──┐                                                 |
 |                       ├──> [ epoll_pwait2 ] ──> [ std::from_chars ]     |
 |  [ signalfd (UNIX) ] ─┘            │                     │              |
 |                                    │                     v              |
 |                        [ Zero-Alloc Hot Path ] ──> [ TTM Math Engine ]  |
 +------------------------------------+------------------------------------+
                                      │
                                      v (write: STDOUT_FILENO)
         systemd --user Sandbox (cgroups v2: MemoryMax=16M, Seccomp BPF)