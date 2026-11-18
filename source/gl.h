// OpenGL include shim shared by C and C++ translation units.
#pragma once

#if defined(__SWITCH__)
#include <glad/glad.h>
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif
