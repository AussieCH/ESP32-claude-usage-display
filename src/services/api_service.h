#pragma once
#include "../models/usage_data.h"
#include <stddef.h>

// Fetches usage from https://claude.ai/api/organizations/{orgId}/usage.
// sessionKey : Claude.ai session cookie value (from browser DevTools)
// orgId      : cached org UUID buffer — auto-discovered and written if empty
bool apiFetch(UsageData& out, const char* sessionKey, char* orgId, size_t orgIdLen);

// Fetches usage from a local/Tailscale usage proxy (see proxy/README.md).
// url   : full proxy endpoint, e.g. http://192.168.1.10:8787/usage
//         or https://<node>.<tailnet>.ts.net/usage
// token : optional Bearer token (empty string = no auth header)
// force : if true, request a cache-bypassing refresh (appends ?force=1)
// HTTPS URLs are verified against the built-in ISRG Root X1 CA.
bool apiFetchProxy(UsageData& out, const char* url, const char* token, bool force = false);
