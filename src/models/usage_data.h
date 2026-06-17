#pragma once
#include <stdint.h>
#include <stdbool.h>

struct UsageBlock {
    uint8_t utilization;   // 0–100 usage percentage
    char    resetsAt[32];  // ISO 8601 reset timestamp, empty if unknown
    bool    available;     // false if this tier is not on the account plan
};

struct UsageData {
    bool       valid;
    char       timestamp[20];  // device uptime when last fetched, e.g. "T+00:05:23"
    UsageBlock fiveHour;
    UsageBlock sevenDay;
    UsageBlock sevenDayOpus;
};
