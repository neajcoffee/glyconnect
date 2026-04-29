#pragma once

extern bool isBLEMode;

// Appelé dans setup() après loadSettingsFromFile()
void glucosePreSetup();

// Appelé dans setup() si isBLEMode, après bgDisplayManager.setup()
void glucoseSetupBLE();

// Appelé dans loop()
void glucoseLoop();
