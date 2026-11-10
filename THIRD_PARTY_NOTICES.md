# Third-party notices

This repository contains no third-party source code or binary assets.

Optional components you may link or run it against (not bundled; check each upstream license
before redistributing a product that includes them):

- TensorFlow Lite / LiteRT C library, when `MAGIC_PORTAL_WITH_TFLITE=ON`.
- ViroCore / ViroReact (ReactVision), which the Android adapter is compiled into.
- A person-segmentation model file of your choice. No model is included; record its license
  and provenance here when you pick one.

The files under `tests/viro_stubs` and `tests/fake_tflite` are test doubles written for this
project; they are not copies of the libraries they imitate.
