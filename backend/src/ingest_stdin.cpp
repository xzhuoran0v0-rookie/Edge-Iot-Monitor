#include <StorageEngine.h>

#include <cstdlib>
#include <iostream>
#include <regex>
#include <string>

static bool extractString(const std::string &json, const char *key, std::string *out)
{
    const std::regex re(std::string("\"") + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (!std::regex_search(json, m, re))
        return false;
    *out = m[1].str();
    return true;
}

static bool extractNumber(const std::string &json, const char *key, double *out)
{
    const std::regex re(std::string("\"") + key + "\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
    std::smatch m;
    if (!std::regex_search(json, m, re))
        return false;
    try
    {
        *out = std::stod(m[1].str());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

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

        std::string deviceId;
        double tsNum = 0;
        double temp = 0;
        double hum = 0;
        double pres = 0;

        if (!extractString(line, "device_id", &deviceId) ||
            !extractNumber(line, "timestamp", &tsNum) ||
            !extractNumber(line, "temperature", &temp) ||
            !extractNumber(line, "humidity", &hum))
        {
            std::cerr << "Skipping line (missing required keys): " << line << "\n";
            failCount++;
            continue;
        }

        const std::string timestamp = std::to_string(static_cast<unsigned long long>(tsNum));

        bool ok = true;
        ok = ok && insertMetric(engine, deviceId, timestamp, "temperature", temp, "C");
        ok = ok && insertMetric(engine, deviceId, timestamp, "humidity", hum, "%");

        if (extractNumber(line, "pressure", &pres))
            ok = ok && insertMetric(engine, deviceId, timestamp, "pressure", pres, "hPa");

        if (ok)
            okCount++;
        else
            failCount++;
    }

    std::cerr << "Done. ok=" << okCount << " fail=" << failCount << "\n";
    return failCount == 0 ? 0 : 1;
}

