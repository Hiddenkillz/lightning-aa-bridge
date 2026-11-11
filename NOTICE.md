# Third-party components

The bridge software is released under the GNU GPL v3 (see `LICENSE`). It builds against or embeds:

| Component | Use | Licence |
|---|---|---|
| [aasdk](https://github.com/f1xpl/aasdk) (and its Android Auto protobuf definitions) | AA protocol on both sides | GPL v3 |
| GStreamer | video pipeline | LGPL v2.1 |
| BlueZ (via D-Bus) | Bluetooth | GPL v2 (used as a system service) |
| OpenSSL | TLS inside the AA session | Apache 2.0 |
| Protocol Buffers | AA messages | BSD 3-clause |
| Boost | aasdk dependency | BSL 1.0 |
| stb_image (`src/ui/third_party`) | placeholder art decode | public domain / MIT |
| DejaVu Sans Bold (`assets/`) | on-screen text | DejaVu licence (Bitstream Vera + public domain) |
| Material Design Icons (`assets/icons`) | UI icons | Apache 2.0 |

Not included, on purpose: Android Auto device certificates (see `README.md`), Bluetooth adapter firmware (use the Pi OS / linux-firmware packages).
