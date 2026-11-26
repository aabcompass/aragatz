# Aragatz: конечный захват DDR → PS Ethernet → ПК

Приложение предназначено для Zynq-7000 XC7Z035, **Vivado/Vitis 2023.1,
standalone, lwIP 2.1.3 raw TCP, IPv4**. Адреса периферии берутся из
сгенерированного `xparameters.h` через существующий `aragatz_hw.h`.
Существующие RTL, PS preset и приложение `ps_sw/baremetal` сохраняются.

**Статус:** исходники и программные проверки подготовлены; сборка ARM
в Vitis и аппаратные echo/capture/throughput ещё не выполнены в этой среде.
[Отчёт о проверках](../../docs/lwip_capture_validation.md) отделяет выполненные
тесты на ПК от приёмки платы.

## 1. Три независимых приложения

| Приложение | Для чего запускается |
|---|---|
| Аппаратная регрессия из `ps_sw/baremetal` | Generator, Capture, AXI HP0, DDR, преамбула и COUNTER |
| Штатный `lwIP Echo Server` из Vitis 2023.1 | GEM/MDIO/KSZ9031/RGMII/link и двусторонний TCP |
| Приложение из этого каталога | Завершённый захват, cache maintenance, неизменный буфер, TCP и проверка файла |

Проблема поддержки KSZ9031 в адаптере не является ошибкой ядра Capture.
Результат echo также не заменяет регрессию PL. Сначала отдельно квалифицируют
сеть, затем запускают сквозной сценарий. Для измерения сети существует ещё
отдельный штатный `lwIP TCP Perf Client`; он не использует Capture.

Не добавляйте `hw_tests.c` и `ddr_tests.c` в сетевое приложение.
Для сети удобно создать отдельную platform/domain на той же XSA,
чтобы регенерация сетевого BSP не меняла контрольную аппаратную сборку.

## 2. Аппаратная платформа Vivado

Используйте XSA **от того же bitstream, на котором пройдена аппаратная
регрессия**. Если XSA экспортируется заново, откройте существующий проект
в Vivado 2023.1, используйте его BD и `scripts/zynq_ps_config.tcl`, затем
экспортируйте hardware с согласованным bitstream. Не запускайте новое
board automation с чужим DDR preset.

В существующей конфигурации ENET0 использует MIO 16…27, MDIO — MIO 52…53;
ENET1 выключен. PHY платы — KSZ9031 с RGMII. Реальные reset/strap/skew
сверяются по схеме платы; адаптер не угадывает их.

Сохраните XSA, bitstream и сгенерированные `xparameters.h`,
`xparameters_ps.h` вместе с UART аппаратной регрессии. Сетевое приложение
использует макросы:

- `XPAR_CAMERA_CAPTURE_BD_0_BASEADDR/HIGHADDR`;
- `XPAR_CAMERA_TEST_GEN_BD_0_BASEADDR/HIGHADDR`;
- `XPAR_PS7_DDR_0_S_AXI_BASEADDR/HIGHADDR`;
- адрес GEM0 из `XPAR_XEMACPS_0_BASEADDR` либо штатного эквивалента;
- настройки UART/CPU/timer из BSP.

В существующем gitlink Generator указан commit
`6bd968297201383d788fcd744fac085e7f2e424b`; на 26 ноября 2026 GitHub API
вернул для него `No commit found`. Для сборки PS на имеющейся XSA скачивать
RTL submodules не требуется. Чистую пересборку Vivado выполняют с доступными
**точными** checkout рабочей регрессии; заменять этот pin текущим `main`
генератора без проверки нельзя. Capture pin
`a2c39d000b85522a4decb5b4f08cc549dc4ea031` сохранён.

## 3. Сначала штатный echo и KSZ9031

В Vitis 2023.1 создайте platform из XSA, standalone domain на
`ps7_cortexa9_0`, затем приложение **lwIP Echo Server**.
В BSP используйте raw API, TCP, IPv4, статический адрес, без DHCP и IPv6.
Для PHY требуется `linkspeed=autodetect`.

В штатном адаптере этой версии отсутствует отдельная процедура KSZ9031.
Приложенный пользователем файл `.9031` также её не содержит и использует
старый API. Подробный разбор, точные hash исходников и команды установки:
[KSZ9031 / Vitis 2023.1](../../docs/ksz9031_vitis_2023_1.md).

Примените к **собираемому сетевому BSP**:

1. Adapter overlay для `xemacpsif_physpeed.c`, `xemacpsif_hw.c`, `xadapter.c`.
2. Driver overlay для ограниченного ожидания в `XEmacPs_PhyRead/Write`.
3. Clean/Build BSP и echo-приложения; проверьте пути исходников в build log.

Тело echo остаётся штатным. Не переносите сюда управление Capture.
Пример адресов изолированного стенда: плата `192.168.10.10/24`,
ПК `192.168.10.20/24`. В echo они задаются в его `main.c`; в приложении
захвата — в `site_config.h`. MAC должен быть уникальным.

После программирования PL/PS и запуска echo сохраните UART:
MDIO-адрес 0…31, ID `0022:162x`, actual link speed и full duplex.
Проверьте реальный TCP-порт в `echo.c` (обычно 7), затем на ПК:

```bash
python3 host/aragatz_client.py echo 192.168.10.10 --port 7
```

Проверьте отключение/возврат кабеля и повторный echo. Для продолжения нужен
успешный результат на плате. `1000` в preset или capabilities PHY не заменяет
измеренного negotiated link. Поддерживаются также 100/10 full duplex;
фактическую скорость фиксируют в протоколе испытаний.

## 4. Собрать приложение выгрузки

Создайте **другое** приложение из echo-шаблона с тем же квалифицированным
сетевым BSP. Исключите шаблонные `main.c`, `echo.c`,
`platform_zynq.c`, замените platform headers файлами этого каталога.
Штатный `platform.c` можно оставить: в 2023.1 его реализации предназначены
для MicroBlaze/PPC и не определяют ARM entry points.

Список C-исходников приложения:

```text
main.c
capture.c
protocol.c
server.c
platform_zynq.c
platform_poll.c
phy/phy_diag.c
```

Include paths: текущий каталог и `../baremetal/include`; BSP include path
задаётся Vitis. Header `phy/phy_diag.h` включается относительно каталога
приложения. Не добавляйте в app исходники `phy/tests`, PS tests, файлы
BSP overlay или stub-заголовки.

`platform_zynq.c` основан на шаблоне 2023.1. Timer ISR выставляет флаги;
основной цикл последовательно обслуживает TCP timers, RX, PHY/link,
ограниченный шаг Capture и отправку. Проверка отсутствующего symbol
`aragatz_ksz9031_read_speed` при линковке указывает на неподключённый BSP overlay.
Глобальный `echo_netif`, ожидаемый шаблоном, определён в `main.c`.

Проверьте сгенерированный `lwipopts.h`: `NO_SYS=1`, `LWIP_TCP=1`,
`LWIP_IPV4=1`, `LWIP_IPV6=0`, `LWIP_DHCP=0`.
Для первого прохода сохраните работающие настройки MSS/windows/pools из echo.
Для производительности позже сравнивайте с отдельным TCP Perf BSP,
меняя один параметр за раз и сохраняя `lwipopts.h`.

## 5. Зарезервировать DDR до линковки

Фиксированный резерв проекта:

```text
[0x10000000, 0x10100000)    1 МиБ
payload + 256 байт suffix guard <= 1 МиБ
```

Он должен быть доступен через HP0 и физически существовать на плате.
Диапазон DDR в `xparameters.h` — конфигурация, а не измерение ёмкости.
CPU1 и другие мастера не должны записывать в этот резерв.

Сначала создайте обычный `lscript.ld` в Vitis. Из корня репозитория:

```bash
python3 tools/reserve_capture_ddr.py /path/to/app/src/lscript.ld /path/to/app/src/lscript_capture.ld
```

Выберите новый `lscript_capture.ld` в настройках линкера приложения.
Скрипт ограничивает выбранную DDR MEMORY region ниже Capture и добавляет
три абсолютных symbol. При неоднозначном имени используйте
`--region <точное_имя_DDR_region>`. Выражения/INCLUDE в MEMORY намеренно
отклоняются: такие linker scripts требуют ручного разбора, а не подстановки
по неполному совпадению. Исходный linker script не перезаписывается.

После сборки:

```bash
python3 tools/check_capture_elf.py /path/to/app/Debug/aragatz_capture.elf
```

Требуется `ELF CHECK PASS`. Проверяются PT_LOAD по physical/virtual addresses,
все allocated sections, включая NOBITS heap/stacks, и границы резерва.
Дополнительно просмотрите map: lwIP pools/GEM DMA descriptors, heap и все
стеки должны принадлежать разрешённой памяти. ELF-проверка не видит
произвольные физические адреса внешних DMA или память другого CPU.

В runtime проверяются точные границы резерва и соответствие BSP.
Они не заменяют post-link проверку. Linker symbols требуются даже при
`ARAGATZ_ENABLE_CAPTURE=0`, поскольку модуль захвата включён в приложение.

## 6. Первый конечный захват

Сначала оставьте `ARAGATZ_ENABLE_CAPTURE=0` и проверьте подключение:

```bash
python3 host/aragatz_client.py status 192.168.10.10
```

После echo PASS, проверки карты/ELF и согласования XSA добавьте compiler
define `ARAGATZ_ENABLE_CAPTURE=1`, пересоберите app и снова проверьте ELF.
Запускайте на чистом IDLE/END с нулевыми ошибками Capture. Если предыдущее
приложение оставило активный захват, сначала завершите/восстановите его
штатным способом; новое приложение не перехватывает работающий DMA.

Первый малый тест, затем основной:

```bash
python3 host/aragatz_client.py capture 192.168.10.10 --frames 64 --prev 16 --fragments 1 --output capture_one.bin
python3 host/aragatz_client.py capture 192.168.10.10 --frames 64 --prev 16 --fragments 16 --output capture.bin
python3 host/aragatz_client.py verify capture.bin
```

В основном примере ожидаются **788480 байт** и **98304 слова COUNTER**.
Это расчёт размера, не результат запуска платы. Клиент создаёт
`.bin` и JSON-метаданные, проверяет CRC32, все преамбулы и все слова,
сохраняет и перечитывает результат, затем посылает прикладной ACK.
Существующие файлы не заменяются без `--overwrite`.

Последовательность PS: подготовка/flush → START → FILL → генератор →
END с завершёнными AXI → остановка генератора → invalidate → CRC →
неизменный RETAINED → TCP COPY → подтверждение клиента.
Подготовка, cache и CRC выполняются порциями до 4 КиБ.
В UART отдельно выводятся времена capture/cache/CRC; они включают
обслуживание основного цикла и не являются чистым временем одной инструкции.

## 7. Обрыв соединения и ошибки

Отключение TCP не освобождает буфер и не останавливает начатый конечный
захват. Подключитесь снова, запросите STATUS. При `state=2` используйте
полученный ID, например:

```bash
python3 host/aragatz_client.py fetch 192.168.10.10 1 --output recovered.bin
```

Число `1` здесь пример: берите фактический ID из STATUS.
Успешный ACK освобождает метаданные; после него повторный FETCH недоступен.
Benchmark выполняет повторы **до** финального ACK.
Потеря ответа на ACK оставляет неопределённость: STATUS покажет,
сохраняется ли буфер, а файл можно проверить локально.

FAULT не очищается сетевой командой. Сохраните UART, CAP_STATUS/CAP_ERRORS
и reason; при невозможности завершить AXI выполняется согласованный
reset/reprogram. Buffer не переиспользуется после timeout.
При ошибке CRC/COUNTER ACK не отправляется.

Полный [контракт протокола v1](../../docs/capture_tcp_v1.md).
Используйте доверенный лабораторный сегмент сети.

## 8. Измерить скорость и выполнить проверки

```bash
python3 host/aragatz_client.py bench 192.168.10.10 --frames 64 --prev 16 --fragments 16 --repeats 100 --output bench_seed.bin --report bench.json
python3 tests/run_host_checks.py
```

[Методика Ethernet](../../docs/ethernet_measurement.md) объясняет отдельный
PS→PC iperf2-тест, FETCH benchmark, единицы и влияние проверки/диска.
[Host README](../../host/README.md) описывает CLI, сохранение и JSON.

После программных тестов на плате обязательно проверить обычный capture,
обрыв посередине, FETCH после обрыва, запрет START при RETAINED, неверный ACK,
отключение/возврат кабеля и правильный отказ при аппаратной ошибке.
Сохраните commit, XSA hash, `git submodule status`, BSP settings,
`lwipopts.h`, ELF/map, UART и JSON клиента.

Входной темп `200 МГц × 24/125 × 8` равен **307,2 МБ/с**.
Полоса 1 GbE до накладных расходов — 125 МБ/с.
Измерение полезной скорости нужно для времени выгрузки и выбора будущего
входного режима; непрерывную полную передачу текущего потока эта версия
не обещает.
