# Aragatz Ethernet — восстановленная версия TCP v1

Статус: черновик исходников, **не собран Vitis и не проверен на плате**.
Не изменяет RTL, PS preset или ps_sw/baremetal. Протокол несовместим с
утраченным вариантом заголовка 192 байта: здесь 64 байта.

## 1. Echo и KSZ9031

Использовать существующую XSA от работающей аппаратной регрессии.
В Vitis 2023.1 создать standalone lwIP Echo Server (Cortex-A9, raw API,
IPv4 static, UART0 115200). GEM0/RGMII и DDR оставить по существующему preset.
ПК: 192.168.10.20/24, плата: 192.168.10.10/24, уникальный MAC.
Проверить MDIO ID1=0022, ID2 & FFF0=1620, адреса 0..31, link и скорость/duplex.
Штатный адаптер может ошибочно распознать PHY как Marvell и пропустить адрес 0.

ksz9031.h — вспомогательный код, **не подключён автоматически к BSP**.
В сгенерированном lwip213/src/contrib/ports/xilinx/netif/xemacpsif_physpeed.c
(найти фактический путь) интегрировать ksz9031_find и ksz9031_negotiate
перед fallback на другие PHY. Обёртки MDIO должны проверять результат
XEmacPs_PhyRead/Write и иметь ограниченное ожидание GEM idle; сами BSP
функции следует проверить на бесконечные ожидания. delay принимает миллисекунды.
Полученную скорость передать штатной ветке настройки GEM clock и MAC speed.
Не вызывать generic Marvell page accesses для KSZ9031.
Сверить ранее работавшие модификации адаптера, reset и RGMII skew по схеме;
не записывать произвольные MMD значения. При потере link повторно согласовать
скорость и настроить GEM или перезапустить приложение (автоматическое
восстановление в этом черновике не реализовано).
Сохранить diff BSP; после regenerate он может исчезнуть.
Сначала добиться Echo PASS:

    python3 host/aragatz_client.py echo 192.168.10.10 --port 7

Порт проверить в фактическом echo.c. Не переходить к Capture до PASS.

## 2. Сборка приложения

Создать отдельное приложение из того же шаблона Echo.
Заменить main.c и исключить echo.c, сохранить platform.c, platform_zynq.c,
platform_config.h и штатные BSP. Добавить main.c, site_config.h из этого
каталога, include path ../baremetal/include. Не добавлять hw_tests.c,
ddr_tests.c или mock BSP. NO_SYS=1, TCP включён, IPv4, без DHCP.
Проверить имена platform flags/EMAC macro по шаблону 2023.1.
Адреса регистров из xparameters.h через aragatz_hw.h.

В lscript.ld ограничить DDR приложения ORIGIN=0x00100000, LENGTH=0x03F00000
(сохранить остальные секции). Проверить ELF/map: все секции, heap, стеки,
lwIP pool и GEM DMA вне [0x10000000,0x10100000). Убедиться в HP0 mapping,
реальной ёмкости DDR, выравнивании и отсутствии других DDR writers.
Проверка stack в коде не заменяет карту линковки.

Сначала ARAGATZ_ENABLE_CAPTURE=0 (по умолчанию), проверить STATUS на порту 5001.
После проверки карты и регрессии задать compiler symbol ARAGATZ_ENABLE_CAPTURE=1.
Установить IP/MAC в site_config.h под стенд. Изолированная сеть без доступа
из интернета: протокол не содержит аутентификации.

## 3. Захват и восстановление

    python3 host/aragatz_client.py status 192.168.10.10
    python3 host/aragatz_client.py capture 192.168.10.10 --frames 64 --prev 16 --fragments 16 --output capture.bin
    python3 host/aragatz_client.py verify capture.bin

Ожидается 788480 байт, 98304 слова COUNTER. ACK после CRC и полной проверки
файла освобождает DDR. При обрыве STATUS, если state=2, затем fetch с id:

    python3 host/aragatz_client.py fetch 192.168.10.10 1 --output recovered.bin

FAULT требует диагностики UART и reset/reprogram PL; ошибки не очищаются
автоматически. После reset прежний capture ID недействителен.

## 4. Измерения

    python3 host/aragatz_client.py bench 192.168.10.10 --repeats 100 --output bench.bin --report bench.json

bench.bin и его метаданные перезаписываются при каждом успешном цикле.
Отчёт содержит время приёма payload, START→payload и полный цикл с проверкой,
записью, перечитыванием и ACK. Скорость — суммарные байты / суммарное время.
Время payload может быть занижено из-за уже заполненных TCP buffers.
Для независимой оценки сети использовать штатный lwIP TCP Perf Client
PS→PC с iperf2 (на ПК iperf -s -i 5 -w 2M), не iperf3.
Записать скорость/duplex, длительность, TCP настройки, версии BSP и логи.
200 МГц × 24/125 × 8 = 307.2 MB/s вход; 1 GbE максимум 125 MB/s
до накладных расходов. Непрерывная полная передача невозможна при этом темпе.

## 5. Проверки

    python3 -m unittest discover -s host -p 'test_*.py'

Сохранить git rev-parse HEAD, git submodule status, XSA, xparameters.h,
xparameters_ps.h, system.mss, lwipopts.h, linker map, UART echo и capture,
старые изменения PHY adapter. Сначала 1 fragment, затем 16, затем bench.
Проверить обрыв на середине передачи, FETCH, ошибочный ACK, отказ START
при retained, отрицательный CRC/COUNTER. Сборка ARM, PHY link, DDR cache,
тайминги PL и пропускная способность пока не подтверждены.
