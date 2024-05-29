# Story
The [libsesame2mqtt](https://github.com/js4jiang5/libsesame2mqtt) project is a fork of [SesameSDK_ESP32_with_DemoApp](https://github.com/CANDY-HOUSE/SesameSDK_ESP32_with_DemoApp).
[SesameSDK_ESP32_with_DemoApp](https://github.com/CANDY-HOUSE/SesameSDK_ESP32_with_DemoApp) is an SDK released by CANDY HOUSE officially for ESP32 developers. However, its main purpose is demonstration, and its functionality is not complete. [libsesame2mqtt](https://github.com/js4jiang5/libsesame2mqtt) was created to fill these gaps, providing full control and monitoring capabilities for Sesame 5/ 5 Pro/ Touch/ Touch Pro. Its primary purpose is to serve as a library for another project, [Sesame2MQTT](https://github.com/js4jiang5/Sesame2MQTT). The [Sesame2MQTT](https://github.com/js4jiang5/Sesame2MQTT) project is an external component of ESPHome, allowing users to easily integrate into the Home Assistant platform.

## Other Languages
- [返回中文版](README.md)
- [日本語版](README_JP.md)

## New Functions
Compared to the official original demonstration version, this project has added the following features in addition to the basic locking and unlocking functionality:
- Horizontal calibration for Sesame 5/5 Pro, lock/unlock angle settings, battery level, and current position display.
- Battery level display and update for Sesame Touch/Touch Pro, pairing/unpairing with Sesame 5/5 Pro.
- Generation of QR code. Scanning the QR code with the Sesame APP on a smartphone allows the addition of devices, which is convenient for smartphone control enthusiasts.
- Addition of WiFi station and MQTT client. Developers using ESP32 can use MQTT as a medium to communicate with Home Assistant. Furthermore, MQTT discovery allows Home Assistant to automatically detect all Sesame 5/ 5 Pro/ Touch/ Touch Pro devices.

## Additional Resources
- Visit the [CANDY HOUSE Official Website](https://jp.candyhouse.co/) for more information about the Sesame5 smart lock.

## License
This project is licensed under the MIT License. See the `LICENSE` file for details.

## Acknowledgments
Thank you for your interest in this project and the support of open resources by CANDY HOUSE.
