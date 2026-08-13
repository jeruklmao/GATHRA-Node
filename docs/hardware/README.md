# Production hardware and GPIO map

The files in this directory are the production wiring source of truth:

- `GATHRA.fzz` — editable Fritzing sketch (found and moved from the workspace root)
- `GATHRA_netlist.xml` — exported netlist
- `GATHRA_schem.png` — schematic view
- `GATHRA_bb.png` — breadboard view

The Fritzing ESP32-C3 Super Mini part connector descriptions and netlist resolve to:

| ESP32-C3 | Function | Connection |
|---|---|---|
| GPIO0 | ADC | battery-divider midpoint |
| GPIO1 | output | SX1278 RST |
| GPIO2 | input pull-up | active-low push button to ground |
| GPIO3 | interrupt input | SX1278 DIO0 |
| GPIO4 | SPI SCK | SX1278 SCK |
| GPIO5 | SPI MISO | SX1278 MISO |
| GPIO6 | SPI MOSI | SX1278 MOSI |
| GPIO7 | SPI chip select | SX1278 NSS/CS |
| GPIO10 | DHT data | DHT22 through bidirectional level shifter |
| GPIO20 | sonar trigger | HY-SRF05 TRIG through level shifter |
| GPIO21 | sonar echo | HY-SRF05 ECHO through level shifter |

These values exist only in `include/board_pins.hpp`; they are not dashboard configuration. The old test programs used obsolete DHT/sonar and SX1278 RST/DIO0 pins and are not a wiring source.

## Power relationships

- SX1278: 3.3 V
- ESP32-C3: board supply
- DHT22 and HY-SRF05: 5 V
- 5 V sensor signals: translated to/from 3.3 V through the designed level shifter
- all modules share the designed ground network

The 5 V rail is powered independently of ESP deep sleep. There is no sensor-rail switch GPIO in the design.

## Battery divider

R1 is 10 kΩ from battery positive to the ADC node; R2 is 5 kΩ from the ADC node to ground. Nominal battery voltage is therefore `calibrated ADC mV × 3`. Firmware takes nine calibrated millivolt samples, discards two at each tail, averages the centre five, then applies the editable multiplier and offset.

Battery-only connected-board validation passed on GPIO0. Five consecutive production measurements reported raw ADC 1760–1768, divider voltage 1302–1305 mV, and reconstructed battery voltage 3906–3915 mV using the default factor 1.0 and offset 0 mV. Earlier near-zero readings were taken with the battery intentionally disconnected while USB powered the ESP32 and therefore do not indicate a divider fault. See `../testing.md` for the evidence.

## Button note

GPIO2 is active LOW and is also strapping-related on ESP32-C3. Firmware uses it exactly as designed, debounces it, waits for release before sleep, and enables low-level deep-sleep GPIO wake. A reproducible boot-mode issue while the physical button is held must be treated as a hardware/boot-strap finding; pins must not be moved in software.
