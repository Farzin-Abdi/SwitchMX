// QR codes for the About page (donation addresses, contact links), drawn with NanoVG.
#pragma once
#include <string>

#include "ui.hpp"

// Draws `text` as a QR code filling `r` (square), dark modules on a white card with the standard
// quiet zone, so phone cameras and wallet apps read it straight off the screen.
void drawQr(NVGcontext* vg, Rect r, const std::string& text, float radius = 10);
