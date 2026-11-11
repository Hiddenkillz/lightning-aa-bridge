#pragma once
// Display parameters the head unit (Sync 4) advertises for the video config we use; we hand the same values to the phone so it lays its UI
// out for this display (density decides the dp width, hence which Android Auto layout: narrow vs wide).
struct VideoParams {
    bool valid = false;
    int ref_win_w = 0, ref_win_h = 0;   // visible window (px) of the truck's largest video config = the layout size a real phone gets in this truck
    int density = 160, real_density = 0, viewing_distance = 0, pixel_aspect_ratio_e4 = 0, decoder_additional_depth = 0;
};
