#pragma once
#include <stdint.h>

enum class AutoBaudResult { Waiting, Found, Failed };
bool autobaud_begin();
void autobaud_cancel();
AutoBaudResult autobaud_poll(uint8_t &rate_index);
