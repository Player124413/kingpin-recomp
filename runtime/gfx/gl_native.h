// The platform's modern GL: GL 3.2 core (macOS) or GL ES 3.0 (iOS, Android).
#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__ANDROID__)
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#define KP_GLES 1
#elif defined(__APPLE__) && TARGET_OS_IPHONE
#define GLES_SILENCE_DEPRECATION
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#define KP_GLES 1
#else
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#define KP_GLES 0
#endif

// Shader source prefix for the platform's GLSL dialect.
#if KP_GLES
// (Samplers too: GLSL ES defaults them to lowp, which reads depth too coarsely for the ambient
// occlusion pass: bands across surfaces.)
#define KP_GLSL_VERSION "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
#else
#define KP_GLSL_VERSION "#version 150\n"
#endif

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
