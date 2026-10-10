# Архитектурный аудит подсистемы amdgpu-sentinel

## 1. Инварианты VFS kernfs и атомарность pread()
- Lockless чтение sysfs через pread(fd, buf, 64, 0) со смещением 0.
- Обход блокировки ядра f_pos_lock и исключение TOCTOU гонок.
- Kernfs re-arming без вызова lseek() и без повторных open()/close().

## 2. Домены памяти TTM и математика вытеснения
- TTM_PL_VRAM (GDDR6, ~384 ГБ/с) vs TTM_PL_TT (DDR4-3600, ~50 ГБ/с по PCIe 4.0 x16 ~31.5 ГБ/с).
- Bandwidth Penalty: 384 / 31.5 ≈ 12.2x просадка скорости инференса.
- TTM_ALERT: VRAM > 85% и GTT > 500 MiB.

## 3. Реактор ядра Linux
- epoll_pwait2 + timerfd(CLOCK_MONOTONIC, 1.0s) — нулевой дрейф таймера.
- signalfd4(SIGINT, SIGTERM) — синхронная обработка сигналов без async-signal-unsafe UB.

## 4. Модель изоляции и cgroups v2
- systemd --user, cgroups v2 MemoryMax=16M (реальный RSS ~420 КБ).
- Seccomp BPF фильтр (@system-service + epoll_pwait2).
- Zero-copy экспорт через POSIX Shared Memory (/dev/shm/vram_guard, 64-байтный Seqlock).
