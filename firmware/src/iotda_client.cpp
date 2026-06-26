#include "iotda_client.h"
#include "config.h"
#include <ArduinoJson.h>

#ifndef IOTDA_DEVICE_ID
#define IOTDA_DEVICE_ID DEVICE_ID
#endif

#ifndef IOTDA_SERVICE_ID
#define IOTDA_SERVICE_ID "Environment"
#endif

#ifndef IOTDA_ALARM_SERVICE
#define IOTDA_ALARM_SERVICE "Alarm"
#endif

#ifndef IOTDA_BUZZER_CMD
#define IOTDA_BUZZER_CMD "BuzzerControl"
#endif

void IotdaClient::logPlannedTopics()
{
    Serial.print("[IoTDA] Property report topic: ");
    Serial.println(propertyReportTopic());
    Serial.print("[IoTDA] Command subscribe topic: ");
    Serial.println(commandSubscribeTopic());
}

String IotdaClient::propertyReportTopic()
{
    return "$oc/devices/" + String(IOTDA_DEVICE_ID) + "/sys/properties/report";
}

String IotdaClient::commandSubscribeTopic()
{
    return "$oc/devices/" + String(IOTDA_DEVICE_ID) + "/sys/commands/#";
}

String IotdaClient::commandResponseTopic(const String &requestId)
{
    return "$oc/devices/" + String(IOTDA_DEVICE_ID) + "/sys/commands/response/request_id=" + requestId;
}

String IotdaClient::buildPropertyReport(float temp, float humi)
{
    JsonDocument doc;
    JsonArray services = doc["services"].to<JsonArray>();
    JsonObject service = services.add<JsonObject>();
    service["service_id"] = IOTDA_SERVICE_ID;

    JsonObject properties = service["properties"].to<JsonObject>();
    properties["temperature"] = roundf(temp * 10.0f) / 10.0f;
    properties["humidity"] = roundf(humi * 10.0f) / 10.0f;

    String payload;
    serializeJson(doc, payload);
    return payload;
}

IotdaBuzzerCommand IotdaClient::parseBuzzerCommand(const String &payload)
{
    IotdaBuzzerCommand command;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
        return command;

    const char *serviceId = doc["service_id"] | "";
    const char *commandName = doc["command_name"] | "";
    if (String(serviceId) != IOTDA_ALARM_SERVICE || String(commandName) != IOTDA_BUZZER_CMD)
        return command;

    JsonVariant paras = doc["paras"];
    if (!paras.is<JsonObject>())
        return command;

    String value;
    if (paras["value"].is<const char *>())
        value = paras["value"].as<const char *>();
    else if (paras["value"].is<int>())
        value = String(paras["value"].as<int>());
    else
        return command;

    command.valid = true;
    command.turnOn = (value == "1");
    command.durationMs = paras["duration_ms"] | 0;
    command.pattern = paras["pattern"] | (command.turnOn ? "continuous" : "none");

    if (command.durationMs < 0 || command.durationMs > 30000)
        command.valid = false;

    return command;
}

String IotdaClient::buildCommandResponse(bool ok, const String &result)
{
    JsonDocument doc;
    doc["result_code"] = ok ? 0 : 1;
    doc["response_name"] = "COMMAND_RESPONSE";
    JsonObject paras = doc["paras"].to<JsonObject>();
    paras["result"] = result;

    String payload;
    serializeJson(doc, payload);
    return payload;
}
