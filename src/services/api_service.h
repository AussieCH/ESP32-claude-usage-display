#pragma once
#include "../models/usage_data.h"
#include <stddef.h>

// Fetches usage from https://claude.ai/api/organizations/{orgId}/usage.
// sessionKey : Claude.ai session cookie value (from browser DevTools)
// orgId      : cached org UUID buffer — auto-discovered and written if empty
bool apiFetch(UsageData& out, const char* sessionKey, char* orgId, size_t orgIdLen);
