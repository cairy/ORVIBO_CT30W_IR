/*
 * 参考草图 — 非本仓库主固件
 *
 * 独立 Arduino 示例：将一路红外遥控器的 NEC 码映射为另一设备的 NEC 码。
 * 针对 ESP32-C3 的引脚配置；主工程 ORVIBO CT30W 固件为 ESP8266，见 src/ 与 platformio.ini。
 *
 * 使用 Arduino IDE 打开本文件所在文件夹（文件夹名须为 ir-converter-finally）。
 */
#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// 接收红外信号的引脚
const uint16_t RECV_PIN = 4;  // ESP32-C3特殊配置
// 发送红外信号的引脚
const uint16_t SEND_PIN = 3;

// 按键映射
const uint64_t SOURCE_VOLUP = 0x11B08F7;  // 接收：音量+
const uint64_t TARGET_VOLUP = 0xFD22DD;   // 发送：音量+

const uint64_t SOURCE_VOLDOWN = 0x11B8877;  // 接收：音量-
const uint64_t TARGET_VOLDOWN = 0xFDC23D;   // 发送：音量-

const uint64_t SOURCE_MUTE = 0x11B708F;    // 接收：静音
const uint64_t TARGET_MUTE = 0xFD708F;     // 发送：静音

const uint64_t SOURCE_POWER = 0x11B26D9;   // 接收：电源
const uint64_t TARGET_POWER = 0xFDB04F;    // 发送：电源

const uint64_t SOURCE_AVTV = 0x11B7689;    // 接收：AV/TV
const uint64_t TARGET_AVTV = 0xFD48B7;     // 发送：AV/TV

// NEC重复码的值
const uint64_t REPEAT_CODE = 0xFFFFFFFFFFFFFFFF;

IRrecv irrecv(RECV_PIN);
IRsend irsend(SEND_PIN);

decode_results results;

void setup() {
  Serial.begin(115200);
  Serial.println("Strarting");
  irrecv.enableIRIn();  // 启动接收器
  irsend.begin();       // 启动发射器
  
  Serial.println("IR Signal Converter Ready - No Repeat");
  Serial.println("Mappings configured for volume, mute, power and AV/TV buttons");
  // irsend.sendNEC(TARGET_POWER, 32);
  // delay(30);
  // // irsend.sendNEC(REPEAT_CODE, 0);
  // // delay(85);  // 短暂延迟
  // // irsend.sendNEC(REPEAT_CODE, 0);  // 额外发送一个重复码
  // // 方法2：手动发送NEC重复码的原始波形
  // uint16_t necRepeat[3] = {9000, 2250, 600};  // 经典NEC重复码波形
  // irsend.sendRaw(necRepeat, 3, 38);  // 38kHz载波
  // delay(70);  // 短暂延迟
  // irsend.sendRaw(necRepeat, 3, 38);  // 38kHz载波
}

void loop() {
  if (irrecv.decode(&results)) {
    // 打印接收到的信号信息
    Serial.print("Received IR Code: ");
    serialPrintUint64(results.value, 16);
    Serial.println();

    uint64_t targetCommand = results.value;

    // 检查并转换特定按键
    if (targetCommand == SOURCE_VOLUP) {
      Serial.println("Sending Target Button Vol Up");
      delay(40);
      irsend.sendNEC(TARGET_VOLUP, 32);  // 使用NEC协议，32位
    } 
    else if (targetCommand == SOURCE_VOLDOWN) {
      Serial.println("Sending Target Button Vol Down");
      delay(40);
      irsend.sendNEC(TARGET_VOLDOWN, 32);
    }
    else if (results.value == SOURCE_MUTE) {
      Serial.println("Sending Target Button Mute");
      delay(40);
      irsend.sendNEC(TARGET_MUTE, 32);
    }
    else if (results.value == SOURCE_POWER) {
      Serial.println("Sending Target Button Power");
      delay(50);
      irsend.sendNEC(TARGET_POWER, 32);
      delay(30);
      // irsend.sendNEC(REPEAT_CODE, 0);
      // delay(85);  // 短暂延迟
      // irsend.sendNEC(REPEAT_CODE, 0);  // 额外发送一个重复码
      // 方法2：手动发送NEC重复码的原始波形
      uint16_t necRepeat[3] = {9000, 2250, 600};  // 经典NEC重复码波形
      irsend.sendRaw(necRepeat, 3, 38);  // 38kHz载波
      delay(70);  // 短暂延迟
      irsend.sendRaw(necRepeat, 3, 38);  // 38kHz载波
    }
    else if (results.value == SOURCE_AVTV) {
      Serial.println("Sending Target Button AV/TV");
      delay(50);
      irsend.sendNEC(TARGET_AVTV, 32);
    }
    irrecv.resume();  // 准备接收下一个值
  }
}
