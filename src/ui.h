#pragma once

namespace ui {

void begin();
void loop();

// Serial helpers, safe from any task. They only set flags that ui::loop() acts on.
// Dumps the next finished frame over USB serial as "SHOT w h" plus raw RGB565 bytes.
void requestShot();
// Acts as a touch at these screen coordinates.
void injectTap(int x, int y);

}  // namespace ui
