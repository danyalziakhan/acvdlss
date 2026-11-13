#pragma once

#include <string>

// acvdlss.ini next to the game's exe.

void config_open();
// Settings were moved from ReShade.ini this start.
bool config_took_over();
int config_int(const char *section, const char *key, int fallback);
std::string config_string(const char *section, const char *key, const char *fallback);
void config_set(const char *section, const char *key, const char *value);
void config_set(const char *section, const char *key, int value);
