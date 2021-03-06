#include <stdlib.h>
#include <stdio.h>

#include "glplatform.h"
#include "gltext.h"
#include "glcore.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <uchar.h>
#include <math.h>

void on_destroy(struct glplatform_win *win)
{
	glplatform_destroy_window(win);
}

#ifdef _WIN32
int CALLBACK WinMain(
	_In_ HINSTANCE hInstance,
	_In_ HINSTANCE hPrevInstance,
	_In_ LPSTR     lpCmdLine,
	_In_ int       nCmdShow)
#else
int main()
#endif
{
	gltext_font_t font;

	struct glplatform_win_callbacks cb;
	memset(&cb, 0, sizeof(cb));
	cb.on_destroy = on_destroy;

	const char32_t *charset = U"abcdefghijklmnopqrstuvwxyz"
		U"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		U"0123456789"
		U" '\"`~!@#$%^&*()_+;/?.>,<={}[]\u2122";

	if (!glplatform_init()) {
		fprintf(stderr, "Failed to initialize GL window manager\n");
		exit(-1);
	}

	struct glplatform_win *win = glplatform_create_window("Simple text test", &cb, NULL, 1024, 256);
	if (!win) {
		fprintf(stderr, "Failed to create OpenGL window\n");
		exit(-1);
	}
	glplatform_gl_context_t ctx = glplatform_create_context(win, 3, 3);
	if (!ctx) {
		fprintf(stderr, "Failed to create OpenGL context\n");
		return 0;
	}
	glplatform_make_current(win, ctx);
	glplatform_glcore_init(3, 3);

	font = gltext_font_create(charset,
		gltext_get_typeface(TTF_PATH "LiberationSans-Regular.ttf"),
		20, false);

	if (!font) {
		fprintf(stderr, "Failed to create font\n");
		exit(-1);
	}
	glplatform_show_window(win);

	while (glplatform_process_events()) {
		int width, height;
		width = win->width;
		height = win->height;
		glViewport(0, 0, width, height);
		glClearColor(0,0,0,1);
		glClear(GL_COLOR_BUFFER_BIT);

		const char32_t *str = U"The quick brown fox jumps over the lazy dog()'\"0123456789`~!@#$%^&*()_+;/?.>,<={}[]\u2122\\";
		int sz = 0;
		while (str[sz]) sz++;
		struct gltext_glyph_instance *r = gltext_prepare_render(font, sz);

		const struct gltext_glyph *g_prev = NULL;
		float x_pos = 0;
		float y_pos = 0;
		struct gltext_color color = {
			.r = 1,
			.g = 1,
			.b = 1,
			.a = 1
		};
		int num_chars = 0;
		for (int i = 0; i < sz; i++) {
			const struct gltext_glyph *g_cur = gltext_get_glyph(font, str[i]);
			if (!g_cur)
				continue;
			x_pos += gltext_get_advance(g_prev, g_cur);
			r[num_chars].pos[0] = floorf(x_pos);
			r[num_chars].pos[1] = y_pos;
			r[num_chars].w = g_cur->w;
			num_chars++;
			g_prev = g_cur;
		}
		x_pos += gltext_get_advance(g_prev, NULL);
		int x_posi = x_pos;
		float mvp[16] = {
			2.0f/width,0,0,0,
			0,-2.0f/height,0,0,
			0,0,1,0,
			-1 + ((width - (int)x_pos)/2)*(2.0f/width),1 + (height/2)*(-2.0f/height),0,1};
		gltext_submit_render(&color, num_chars, mvp);
		glplatform_swap_buffers(win);

		if (glplatform_get_events(true) < 0)
			break;
	}
	return 0;
}
