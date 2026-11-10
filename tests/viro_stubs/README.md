# ViroCore / GLES stub headers

These headers are **test doubles**. They reproduce only the parts of the ViroCore (ReactVision)
API and OpenGL ES that the adapter in `adapters/viro` calls, so the adapter's own logic can be
compiled and unit-tested without an Android toolchain, an AR session, or a GPU.

They are written from the API surface the adapter uses; they are not copied from ViroCore.
Passing the stub-based tests therefore shows the adapter logic is self-consistent. It does
**not** show that the signatures match the ViroCore version you build against - compiling
`adapters/viro` against the real headers is a required integration step (see
`docs/DEVICE_VALIDATION.md`).
