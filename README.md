# 緣起
[libsesame2mqtt](https://github.com/js4jiang5/libsesame2mqtt) 這個專案是 [SesameSDK_ESP32_with_DemoApp](https://github.com/CANDY-HOUSE/SesameSDK_ESP32_with_DemoApp) 的分支。<br>
[SesameSDK_ESP32_with_DemoApp](https://github.com/CANDY-HOUSE/SesameSDK_ESP32_with_DemoApp) 是由 CANDY HOUSE 官方所釋出的專門給 ESP32 開發者使用的 SDK，但是其主要目的是展示，功能並不完整。[libsesame2mqtt](https://github.com/js4jiang5/libsesame2mqtt) 便是為了彌補其不足而產生，針對 Sesame 5/ 5 Pro/ Touch/ Touch Pro 的控制與監測提供完整的功能。其主要目的是作為另一個專案 [Sesame2MQTT](https://github.com/js4jiang5/Sesame2MQTT) 的函式庫。[Sesame2MQTT](https://github.com/js4jiang5/Sesame2MQTT) 專案是一個 ESPHome 的外部元件，能讓使用者輕鬆的整合進 Home Assistant 平台。 

## 多語言版本
- [English version](README_EN.md)

## 新增功能
相較於官方原始的展示版，除了基本的上鎖解鎖功能外，此專案新增功能如下
1. Sesame 5/5 Pro 的水平校正，上鎖/解鎖角度設定，電池電量與目前位置顯示。
2. Sesame Touch/Touch Pro 的電池電量顯示與更新，配對/解除 Sesame 5/5 Pro。
3. 產生 QR code。手機的 Sesame APP 掃描 QR code 便能加入設備，方便手機控制愛好者使用。
4. 增加 WiFi station 與 MQTT client。ESP32 的開發者可以透過 MQTT 作為與 Home Assistant 溝通的媒介。此外 MQTT discovery 更進一步讓 Home Assistant 能自動搜尋到所有的 Sesame 5/ 5 Pro/ Touch/ Touch Pro 等設備。

## 額外資源
- 請訪問 [CANDY HOUSE 官方網站](https://jp.candyhouse.co/) 了解更多關於 Sesame5 智能鎖的信息。

## 許可證
本專案採用 MIT 許可證。詳情請參見 `LICENSE` 文件。

## 致謝
感謝您對此專案的興趣，以及 CANDY HOUSE 公司對於開放資源的支持。
