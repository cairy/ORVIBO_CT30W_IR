#ifndef AT_HANDLER_H
#define AT_HANDLER_H

#include <Arduino.h>
#include "network/network_config.h"

// AT 指令处理器
class ATHandler {
public:
    ATHandler(NetworkConfigManager& config);
    void init();
    void update();
    void processCommand(const String& command);

    // 单条命令的字节上限。最长的命令是 AT+MQTT=server,port,user,password，
    // 正常也就几十字节；留一倍余量足够，同时把"无限增长"这条路堵死。
    static const size_t COMMAND_MAX_LEN = 128;

private:
    NetworkConfigManager& configManager;
    String commandBuffer;
    // 当前这一行已经超长，正在丢弃剩余字符直到行尾。
    // 串口上任何一段不含换行的字节流（波特率不匹配、接触不良、刷机工具的残留）
    // 以前会让 commandBuffer 一路涨到 OOM 崩溃，而设备正在干别的活时根本看不出原因。
    bool commandOverflow;
    
    void handleWiFi(const String& params);
    void handleMQTT(const String& params);
    void handleQuery();
    void handleRestore();
    void sendResponse(const String& response);
};

#endif // AT_HANDLER_H

