#ifndef GLPLATFORM_PRIV_H
#define GLPLATFORM_PRIV_H

#ifdef _WIN32
#include <windows.h>
#else
#include "glplatform-glx.h"
#endif

struct gltext_renderer;
struct glplatform_context {
	struct gltext_renderer *text_renderer;
#ifdef _WIN32
	HGLRC rc;
#else
	GLXContext ctx;
#endif
};

struct glplatform_context *glplatform_get_context_priv();

#endif
