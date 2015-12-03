#ifndef GLPLATFORM_PRIV_H
#define GLPLATFORM_PRIV_H

#ifdef _WIN32
#include <windows.h>
struct gltext_renderer;
struct glplatform_context {
	struct gltext_renderer *text_renderer;
	HGLRC rc;		
};

struct glplatform_context *glplatform_get_context_priv();

#endif

#endif
