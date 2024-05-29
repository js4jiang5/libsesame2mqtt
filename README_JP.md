# 緣起
[libsesame2mqtt](https://github.com/js4jiang5/libsesame2mqtt) このプロジェクトは [SesameSDK_ESP32_with_DemoApp](https://github.com/CANDY-HOUSE/SesameSDK_ESP32_with_DemoApp) のフォークです。
[SesameSDK_ESP32_with_DemoApp](https://github.com/CANDY-HOUSE/SesameSDK_ESP32_with_DemoApp) はCANDY HOUSE公式によってリリースされた、ESP32開発者向けのSDKですが、その主な目的はデモンストレーションであり、機能は完全ではありません。[libsesame2mqtt](https://github.com/js4jiang5/libsesame2mqtt)  はその不足を補うために生まれ、Sesame 5/ 5 Pro/ Touch/ Touch Pro の制御とモニタリングに完全な機能を提供します。その主な目的は、別のプロジェクト[Sesame2MQTT](https://github.com/js4jiang5/Sesame2MQTT) のライブラリとして機能することです。[Sesame2MQTT](https://github.com/js4jiang5/Sesame2MQTT) プロジェクトはESPHomeの外部コンポーネントであり、ユーザーが Home Assistant プラットフォームに簡単に統合できるようにします。

## 多言語バージョン
- [繁體中文版](README.md)
- [English version](README_EN.md)

## 新機能
公式のデモ版と比較して、基本的な施錠・解錠機能に加えて、このプロジェクトには以下の新機能が追加されました。
- Sesame 5/5 Pro の水平調整、施錠/解錠の角度設定、バッテリー残量と現在位置の表示。
- Sesame Touch/Touch Pro のバッテリー残量表示と更新、Sesame 5/5 Pro とのペアリング/ペアリング解除。
- QRコードの生成。スマートフォンの Sesame APP でQRコードをスキャンすると、デバイスを追加でき、スマートフォンコントロールの愛好家にとって便利です。
- WiFiステーションとMQTTクライアントの追加。ESP32の開発者は、MQTTを介して Home Assistant と通信する媒体として使用できます。さらに、MQTTディスカバリーにより、Home Assistant は自動的にすべての Sesame 5/ 5 Pro/ Touch/ Touch Pro デバイスを検出できます。

## 追加リソース

- セサミ5スマートロックに関する詳細は、[CANDY HOUSE公式ウェブサイト](https://jp.candyhouse.co/)をご覧ください。

## ライセンス

このプロジェクトはMITライセンスのもとで公開されています。詳細は`LICENSE`ファイルをご覧ください。

## 謝辞

このプロジェクトにご興味を持っていただき、オープンソースリソースに対するCANDY HOUSE社のサポートに感謝します。
