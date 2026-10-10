# amdgpu-sentinel

[![Platform](https://img.shields.io/badge/Platform-Arch_Linux-blue.svg?logo=arch-linux)](https://archlinux.org/)
[![Kernel](https://img.shields.io/badge/Kernel-6.x-green.svg?logo=linux)](https://kernel.org/)
[![GPU](https://img.shields.io/badge/GPU-RDNA_2_(Navi_22)-red.svg?logo=amd)](https://www.amd.com/)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-orange.svg?logo=c%2B%2B)](https://en.cppreference.com/)
[![Sandbox](https://img.shields.io/badge/Sandbox-systemd_cgroups_v2-black.svg?logo=systemd)](https://systemd.io/)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Демон мониторинга видеопамяти и телеметрии видеокарт AMD RDNA 2 (Navi 22 / RX 6700 XT) под Linux.

Сервис считывает метрики драйвера `amdgpu` через виртуальную файловую систему `sysfs` (`kernfs`): объем используемой VRAM, системной памяти GTT, процент занятости вычислительных блоков и температуру ядра. Основная задача: детекция вытеснения буферов памяти менеджером DRM TTM при локальной работе LLM (llama.cpp / ROCm).

## Особенности реализации

1. **Чтение через `pread()`:** дескрипторы узлов `/sys/class/drm/card*/device/` открываются один раз при запуске. Чтение выполняется системным вызовом `pread(fd, buf, size, 0)` со смещением 0, что исключает обновление позиции `f_pos` и обходит блокировку `f_pos_lock` в ядре.
2. **Событийный цикл:** период опроса 1.0 с задается аппаратным таймером `timerfd_create(CLOCK_MONOTONIC)` через демультиплексор `epoll_pwait2`.
3. **Обработка сигналов:** сигналы `SIGINT` и `SIGTERM` блокируются в потоке и обрабатываются синхронно через дескриптор `signalfd4`.
4. **Устойчивость к смене состояний питания:** при переходе шины PCIe в режимы энергосбережения (GFXOFF, BACO, Runtime D3cold) драйвер может возвращать `-EAGAIN` или `-EBUSY`. Демон сохраняет последнее валидное значение и выставляет флаг деградации телеметрии.
5. **Потребление памяти:** сборка с флагами `-fno-exceptions`, `-fno-rtti`, LTO и стриппингом дает около 420 КБ RSS при лимите cgroups v2 `MemoryMax=16M`.
6. **IPC через `/dev/shm`:** состояние публикуется в `/dev/shm/vram_guard` через 64-байтную структуру с Seqlock.

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
```

## Управление памятью: TTM и вытеснение в GTT

### Домены памяти (AMD Radeon RX 6700 XT)
* **VRAM (`TTM_PL_VRAM`):** 12 ГБ GDDR6, 192-битная шина, пропускная способность ~384 ГБ/с.
* **GTT (`TTM_PL_TT`):** оперативная память хоста (DDR4-3600 в двухканальном режиме, ~50 ГБ/с). Обмен идет по шине PCIe 4.0 x16 с пределом ~31.5 ГБ/с.

### Штраф по пропускной способности шины
Если модель превышает физический объем видеопамяти, менеджер TTM ядра Linux перемещает буферные объекты (BO) в системную память GTT.

Снижение скорости вычисляется по формуле:

$$\text{Bandwidth Penalty} = \frac{\text{Bandwidth}_{\text{VRAM}}}{\min(\text{Bandwidth}_{\text{PCIe}}, \text{Bandwidth}_{\text{Host RAM}})} \approx \frac{384}{31.5} \approx 12.19\times$$

Падение ПСП в 12.2 раза вызывает задержки при обработке токенов. Демон фиксирует факт вытеснения по предикату:

$$\text{TTM\_ALERT} = (\text{VRAM}_{\text{used}} > 85\%) \land (\text{GTT}_{\text{used}} > 500\,\text{MiB})$$

## Сборка и установка

Требования: Arch Linux / EndeavourOS (ядро 6.x), членство в группах `render` и `video`.

```bash
git clone https://github.com/BannedForWhat/amdgpu-sentinel.git
cd amdgpu-sentinel
make clean && make
make install

# Запуск службы в сеансе пользователя
systemctl --user daemon-reload
systemctl --user enable --now amdgpu-sentinel.service

# Просмотр статуса и журнала
systemctl --user status amdgpu-sentinel.service
journalctl --user -u amdgpu-sentinel.service -f
```

## Параметры изоляции (systemd cgroups v2)

| Директива | Назначение |
| :--- | :--- |
| `MemoryMax=16M` | Жесткий лимит физической памяти. При превышении процесс завершается OOM-киллером группы. |
| `MemoryHigh=12M` | Мягкое ограничение с троттлингом аллокаций. |
| `NoNewPrivileges=yes` | Блокировка повышения привилегий через SUID бинарники. |
| `RestrictAddressFamilies=AF_UNIX` | Отключение сетевых сокетов IPv4 и IPv6 на уровне ядра. |
| `SystemCallFilter=@system-service ...` | Белый список сисколлов (включает `pread64`, `epoll_pwait2`, `signalfd4`). |

## Формат вывода

```text
[2026-10-08T13:48:45.181Z] VRAM:  2.11/11.98 GiB ( 17.6%) | GTT:  0.33/15.63 GiB | GPU:  41% | TEMP: 50.0 C | TTM: OPTIMAL
[2026-10-08T13:48:46.181Z] VRAM:  2.09/11.98 GiB ( 17.5%) | GTT:  0.33/15.63 GiB | GPU:  16% | TEMP: 50.0 C | TTM: OPTIMAL
```

## Ограничения и планы развития

1. **Период опроса 1.0 с:** шаг таймера снижает нагрузку на CPU хоста (около 13 мс процессорного времени за сессию), но не фиксирует импульсные аллокации KV-кэша длительностью менее секунды.
2. **Парсинг kernfs:** чтение виртуальных файлов sysfs требует форматирования строк ядром в буфер страницы памяти.

Планы (v1.1):
- [ ] Переход на чтение через `ioctl(DRM_IOCTL_AMDGPU_INFO)` без участия kernfs.
- [ ] Интеграция eBPF-трейспоинта `tracepoint:amdgpu:amdgpu_ttm_bo_move` для отслеживания миграции страниц по событию.
