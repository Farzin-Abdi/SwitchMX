#pragma once
#include <string>

// Prepares a UTF-8 string for a renderer that draws glyphs left to right with no shaping:
// Arabic/Persian letters are replaced by their joined presentation forms (including lam-alef
// ligatures) and right-to-left runs are reordered into visual order. Text without RTL
// characters is returned unchanged.
std::string shapeForDisplay(const std::string& utf8);
