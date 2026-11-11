#pragma once
// One touch event with ALL fingers, exactly as Android Auto carries it. Coordinates are in whichever space the holder defines
// (touch space from the head unit, or phone space after remapping); `id` is the head unit's pointer id and is preserved end to end.
#include <vector>
struct TouchPointer { int id = 0, x = 0, y = 0; };
struct TouchFrame {
    std::vector<TouchPointer> p;
    int action = 0;     // raw Android action: 0 down, 1 up, 2 move, 3 cancel, 5 pointer-down, 6 pointer-up
    int index = 0;      // index into p of the pointer that changed (for 5 / 6)
};
