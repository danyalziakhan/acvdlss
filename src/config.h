#pragma once

#include <string>

// acvdlss.ini next to the game's exe holds all add-on settings. Created with
// defaults on first start, read at startup, written when a setting changes
// in the overlay.

void config_open();
// True when this start copied the settings over from ReShade.ini.
bool config_took_over();
int config_int(const char *section, const char *key, int fallback);
std::string config_string(const char *section, const char *key, const char *fallback);
void config_set(const char *section, const char *key, const char *value);
void config_set(const char *section, const char *key, int value);
