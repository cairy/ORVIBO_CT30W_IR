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

private:
    NetworkConfigManager& configManager;
    String commandBuffer;
    
    void handleWiFi(const String& params);
    void handleMQTT(const String& params);
    void handleQuery();
    void sendResponse(const String& response);
};

#endif // AT_HANDLER_H

