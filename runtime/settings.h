#pragma once
// hc09.ini: player settings, read once at startup (runtime/settings.cpp).
#include <string>

// Read the settings file (HC09_CONFIG, default hc09.ini in the working
// directory; a missing file is fine) and turn its entries into the HC09_*
// environment variables the runtime reads. A variable already set in the
// environment wins over the file. `root` receives [game] root if the file
// names one and *root is empty (--root wins too).
void SettingsLoad(std::string* root);
