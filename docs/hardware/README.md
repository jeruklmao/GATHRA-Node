# Node hardware documentation

GATHRA_netlist.xml in this directory is an exact archive of the workspace source latest_GATHRA_netlist.xml (SHA-256 25bbca8e105a63c05b4061083f1254afba08fd3ef61e2efd782a28190a6633e8) and is authoritative for the 2.0.0 board.

GATHRA.fzz, GATHRA_bb.png, and GATHRA_schem.png predate the PCF8563/AO3401A redesign and are retained only as stale historical artifacts. They must not be used to wire or review the current board.

## ESP32-C3 GPIO map

| GPIO | Net |
| ---: | --- |
| 0 | Battery ADC |
| 1 | SX1278 RST |
| 3 | SX1278 DIO0 |
| 4 | SX1278 SCK |
| 5 | SX1278 MISO |
| 6 | SX1278 MOSI |
| 7 | SX1278 NSS/CS |
| 8 | PCF8563 SDA |
| 9 | PCF8563 SCL |
| 10 | DHT22 DATA |
| 20 | HY-SRF05 TRIG |
| 21 | HY-SRF05 ECHO |
| 2 | active buzzer; HIGH=on, LOW=off |

There is no ESP push-button GPIO.

## Power topology

The battery powers PCF8563 continuously. AO3401A switches the rest of the Node. Its gate has a 100 kΩ pull-up to off; the push button and PCF8563 INT are open/low paths to on. The ESP controls power only by configuring and clearing RTC timer/alarm interrupt state.

PCF8563 I2C intentionally relies on ESP internal pull-ups while switched electronics are powered. No always-powered external SDA/SCL pull-up is assumed. If field I2C behavior is unreliable, measure rise time and report a hardware finding rather than changing firmware pin assignments.

USB back-powers the ESP32-C3 and cannot validate rail removal. Final validation must remove only Node USB and use the battery/solar circuit.
