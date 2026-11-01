#pragma once
#include <stdint.h>

enum class NetworkState : uint8_t { Unconfigured, Provisioning, Connecting, Online, Failed };
void network_time_init();
void network_request_provisioning();
NetworkState network_state();
const char *network_status_text();
int64_t network_epoch_us(uint64_t monotonic_us); // zero until a real SNTP sync
