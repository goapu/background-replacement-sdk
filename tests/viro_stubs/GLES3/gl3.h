#pragma once
/* Stub: lets tests choose which extensions the "device" reports. */
typedef unsigned int GLenum;
typedef unsigned char GLubyte;
#define GL_EXTENSIONS 0x1F03
#ifdef __cplusplus
extern "C" {
#endif
extern const char *g_stub_gl_extensions;
inline const GLubyte *glGetString(GLenum) {
    return (const GLubyte *)g_stub_gl_extensions;
}
#ifdef __cplusplus
}
#endif
