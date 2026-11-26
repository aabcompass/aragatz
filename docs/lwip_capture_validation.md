# Валидация сетевого приложения Aragatz

Дата: **26 ноября 2026 года**. Результаты ниже относятся к исходникам
этой ревизии ветки `lwip-capture-v1`, формат 32/64 байта, version 1.

## Фактически выполнено на ПК

Общая команда:

```bash
python3 tests/run_host_checks.py
```

| Набор | Результат | Что проверяет |
|---|---:|---|
| Python-клиент | 37 тестов PASS | CRC, geometry, COUNTER/preamble, atomic save/readback, ACK ordering, ошибки диска, TCP loopback, C/Python wire compatibility |
| Контроллер Capture | 15 сценариев PASS | MMIO/cache-модель, устаревший END, readback, ошибки/guard, timeout, STOP в AFTER, повторный writer |
| Raw TCP сервер | 404 проверки PASS | Fragmentation/pbuf chains, ERR_MEM, очереди, TCP/app ACK, retained FETCH, disconnect, ID/geometry, второй клиент |
| Linker/ELF | 23 теста PASS | Выражения/неоднозначность MEMORY, резерв DDR, PT_LOAD VA/PA, NOBITS heap/stacks, точные linker symbols |
| PHY helper | PASS | 10/100/1000, full/half duplex, BMCR/BMSR, startup timeout, частично выполненная настройка |
| MDIO discovery | PASS | Все 32 адреса, отсутствующий/неверный/неоднозначный PHY, ошибки MDIO |
| Драйвер MDIO | PASS | Реальные преобразованные функции, finite poll budget, busy/stuck/последний poll |
| GEM/link recovery | PASS | Неудачный startup, renegotiation, cache скорости, MDIO failure, отсутствие lwIP-вызова из error IRQ |

C-модели собирались системным GCC/cc с `-std=c11 -Wall -Wextra -Werror`.
Python TCP-тесты используют настоящий локальный TCP и модель платы.
Полный вывод запуска: [host_validation_20261126.txt](host_validation_20261126.txt).
Один тест компилирует C-кодировщик, получает его 64-байтовый header
и COUNTER payload и проверяет независимым Python-клиентом.

Также выполнены:

- Сравнение первого вложения со штатным `xilinx_v2023.1`; hash совпал
  после нормализации CRLF. Второе вложение разобрано отдельно: реализации
  KSZ9031 в нём нет.
- Генерация adapter overlay по полным официальным исходникам 2023.1;
  соответствие генерируемых файлов представленному review patch.
- Syntax/type-проверка `server.c` по настоящим hash-checked заголовкам
  lwIP 2.1.3/Xilinx 2023.1.
- Проверка API-сборки `main.c` с capture=0 и =1, capture/protocol/server,
  platform_zynq/platform_poll и штатного platform.c; частичная линковка
  и единственность основных символов. Сетевые заголовки, xtime_l.h
  и platform.c настоящие; BSP/GIC/SCU/ARM cache представлены временными
  явно помеченными shims. Это не целевая сборка.

Дополнительные команды при наличии исходников тега `xilinx_v2023.1`:

```bash
python3 tests/compile_upstream_headers.py --include-root /path/to/lwip-2.1.3/src/include --port-include /path/to/contrib/ports/xilinx/include
python3 ps_sw/lwip_capture/tests/compile_app_api.py --include-root /path/to/lwip-2.1.3/src/include --port-include /path/to/contrib/ports/xilinx/include --standalone-include /path/to/standalone/arm/cortexa9 --echo-platform /path/to/lwip_echo_server/src/platform.c
```

Для полного совпадения путей используйте аргументы `--help`.
Скрипты не скачивают файлы и не принимают неподтверждённую версию заголовков.

## Что остаётся проверить на стенде

| Проверка | Статус |
|---|---|
| Полная сборка приложений/BSP в Vitis 2023.1, ARM ELF и map | Не выполнена в этой среде |
| Действительный PHY address/reset/strap, RGMII timing, link и echo | Требуется плата |
| ARM cache maintenance совместно с AXI HP0 | Требуется плата |
| Реальный capture → END → TCP → CRC/COUNTER на ПК | Требуется плата |
| Обрыв, FETCH, hotplug и отрицательные ACK на плате | Требуется плата |
| Длительная PS→PC пропускная способность | Не измерена |
| Полезная скорость проверенного FETCH и полного цикла | Не измерена |

Результат V2 аппаратной регрессии (9 сценариев, 27582 проверки, 0 ошибок)
получен из сообщения/документации пользователя; этот запуск здесь
не воспроизводился. Он относится к ядру и аппаратному тракту, а не к PHY.
Содержимое `ps_sw/baremetal`, RTL gitlinks и PS preset этой доработкой
не изменяются.

`xemac_add()!=NULL` само по себе не подтверждает исправность аппаратной сети:
в штатных низкоуровневых путях адаптера не все ошибки конфигурации/DMA
полностью поднимаются до этого результата. Поэтому echo остаётся
обязательным отдельным этапом.

Текущие границы: один GEM0, один KSZ9031 RGMII, один TCP-клиент,
доверенная сеть, конечный COUNTER capture, 1…16 фрагментов, DDR 1 МиБ.
PHY reset/MMD skew/FLP/reference clock не меняются без данных платы.

## Приёмочные материалы

Сохраните commit этой ветки и `git submodule status`, hash XSA/bitstream,
`xparameters.h`, `xparameters_ps.h`, `system.mss`, `lwipopts.h`,
linker script, ELF/map и результат `check_capture_elf.py`.
К ним приложите UART аппаратного теста, echo, capture, iperf2,
JSON benchmark и один проверенный `.bin/.json`.

Порядок запуска:
[инструкция приложения](../ps_sw/lwip_capture/README.md),
[KSZ9031](ksz9031_vitis_2023_1.md),
[методика измерения скорости](ethernet_measurement.md).
