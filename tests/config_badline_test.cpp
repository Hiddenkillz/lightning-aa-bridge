#include "Config.h"
#include <cassert>
int main(){ { std::ofstream f("/tmp/aa-bad.conf"); f << "video_width=abc\ncamera_fps=30\nmix_latency_ms=99999999999999999999\nap_ssid=x\n"; }
  Config c = Config::load("/tmp/aa-bad.conf");
  assert(c.camera_fps==30); assert(c.ap_ssid=="x"); assert(c.video_width==Config().video_width); std::cout<<"CONFIG BAD-LINE TEST PASSED\n"; }
