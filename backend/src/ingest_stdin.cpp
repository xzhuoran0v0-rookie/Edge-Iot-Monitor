#include <StorageEngine.h>
#include "nlohmann/json.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

static void usage(const char *argv0)
{
    std::cerr
        << "Usage: " << argv0 << " [--db <path>]\n"
        << "Reads one JSON object per line from stdin (simulate_sensor.py --stdout).\n"
        << "Expected keys: device_id, timestamp, temperature, humidity, (optional) pressure.\n";
}

static bool insertMetric(StorageEngine &engine,
                         const std::string &deviceId,
                         const std::string &timestamp,
                         const char *sensorType,
                         double value,
                         const char *unit)
{
    SensorReading r;
    r.device_id = deviceId;
    r.sensor_type = sensorType;
    r.value = value;
    r.unit = unit;
    r.timestamp = timestamp; // keep epoch string for now
    return engine.insertReading(r);
}

static std::string timestampToString(const json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float())
        return std::to_string(static_cast<unsigned long long>(value.get<double>()));
    throw std::runtime_error("timestamp must be string or number");
}

int main(int argc, char **argv)
{
    std::string dbPath = "sensor.db";

    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        if (arg == "--db" && i + 1 < argc)
        {
            dbPath = argv[++i];
        }
        else if (arg == "-h" || arg == "--help")
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown arg: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    StorageEngine engine(dbPath);
    if (!engine.init())
    {
        std::cerr << "DB init failed: " << dbPath << "\n";
        return 1;
    }

    std::string line;
    int okCount = 0;
    int failCount = 0;

    while (std::getline(std::cin, line))
    {
        if (line.empty())
            continue;

        json payload;
        try
        {
            payload = json::parse(line);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Skipping line (invalid JSON: " << e.what() << "): " << line << "\n";
            failCount++;
            continue;
        }

        if (!payload.contains("device_id") ||
            !payload.contains("timestamp") ||
            !payload.contains("temperature") ||
            !payload.contains("humidity"))
        {
            std::cerr << "Skipping line (missing required keys): " << line << "\n";
            failCount++;
            continue;
        }

        std::string deviceId;
        std::string timestamp;
        double temp = 0;
        double hum = 0;
        try
        {
            deviceId = payload.at("device_id").get<std::string>();
            timestamp = timestampToString(payload.at("timestamp"));
            temp = payload.at("temperature").get<double>();
            hum = payload.at("humidity").get<double>();
        }
        catch (const std::exception &e)
        {
            std::cerr << "Skipping line (bad field type: " << e.what() << "): " << line << "\n";
            failCount++;
            continue;
        }

        bool ok = true;
        ok = ok && insertMetric(engine, deviceId, timestamp, "temperature", temp, "C");
        ok = ok && insertMetric(engine, deviceId, timestamp, "humidity", hum, "%");

        if (payload.contains("pressure"))
        {
            try
            {
                const double pres = payload.at("pressure").get<double>();
                ok = ok && insertMetric(engine, deviceId, timestamp, "pressure", pres, "hPa");
            }
            catch (const std::exception &e)
            {
                std::cerr << "Skipping line (bad pressure field: " << e.what() << "): " << line << "\n";
                failCount++;
                continue;
            }
        }

        if (ok)
            okCount++;
        else
            failCount++;
    }

    std::cerr << "Done. ok=" << okCount << " fail=" << failCount << "\n";
    return failCount == 0 ? 0 : 1;
}
