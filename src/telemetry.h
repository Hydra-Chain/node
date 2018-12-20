#ifndef TELEMETRY_H
#define TELEMETRY_H

std::string TelemetryQGet();
std::string telemetry_adder(const char* fmt, ...);
void SendToTelemetry(const std::string &str, const std::string &params);
void TelemetrySetBlacklisted(std::string blacklist);
std::string ReadTelemetryLogsSync();
void SaveTelemetryLogsSync(std::string logs, bool append);

#endif //TELEMETRY_H
