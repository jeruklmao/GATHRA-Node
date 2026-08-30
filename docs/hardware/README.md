# Node hardware

`include/board_pins.hpp` is the current wiring authority. Runtime configuration
cannot override these pins.

## ESP32-C3 GPIO map

| GPIO | Net |
| ---: | --- |
| 0 | battery ADC |
| 1 | SX1278 RST |
| 3 | SX1278 DIO0 |
| 4 | SX1278 SCK |
| 5 | SX1278 MISO |
| 6 | SX1278 MOSI |
| 7 | SX1278 NSS/CS |
| 8 | PCF8563 SDA |
| 9 | PCF8563 SCL |
| 10 | DHT22 data |
| 20 | HY-SRF05 trigger |
| 21 | HY-SRF05 echo |
| 2 | active-HIGH buzzer |

There is no ESP push-button GPIO.

## Power topology

The battery powers the PCF8563 continuously. An AO3401A switches the ESP32-C3,
radio, sensors, and boost circuitry. The push button and PCF8563 open-drain INT
can pull the MOSFET gate low to power the switched rail. Firmware controls
shutdown by configuring and clearing RTC timer/alarm interrupt state.

PCF8563 I2C uses the ESP32 internal pull-ups while the switched electronics are
powered. USB powers the ESP32-C3 and therefore cannot validate removal of the
battery-switched rail.
