#include "glplatform.h"

#include <wingdi.h>
#include <windowsx.h>

#include "glplatform-wgl.h"

//
// wglGetProcAddress() only works if a WGL context is current so
// we need to create a context *before* we call glplatform_wgl_init().
// So we need to undefine wglMakeCurrent and wglCreateContext to
// we can call these functions directly and break the circular
// dependency.
//
#undef wglMakeCurrent
#undef wglCreateContext

#include "glplatform_priv.h"

static LRESULT CALLBACK PlatformWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

static struct glplatform_win *g_win_list = NULL;
static int g_glplatform_win_count = 0;
static DWORD g_context_tls;
static int g_cursor_count = 1;

static struct glplatform_win *find_glplatform_win(HWND hwnd)
{
	struct glplatform_win *win = g_win_list;
	while (win) {
		if (win->hwnd == hwnd)
			return win;
		win = win->next;
	}
	return NULL;
}

static void retire_glplatform_win(struct glplatform_win *win)
{
	struct glplatform_win *pos = g_win_list;
	while (win) {
		if (pos == win) {
			*(pos->pprev) = pos->next;
			if (pos->next)
				pos->next->pprev = pos->pprev;
			g_glplatform_win_count--;
			break;
		}
		pos = pos->next;
	}
}

static void register_glplatform_win(struct glplatform_win *win)
{
	struct glplatform_win *test = find_glplatform_win(win->hwnd);
	if (test)
		return;
	g_glplatform_win_count++;
	win->next = g_win_list;
	win->pprev = &g_win_list;
	g_win_list = win;
}

#define PRESSED_MASK ((SHORT)0x8000)

bool glplatform_is_button_pressed(struct glplatform_win *win, int button)
{
	switch (button) {
	case 0:
		return GetKeyState(VK_LBUTTON) & PRESSED_MASK;
	case 1:
		return GetKeyState(VK_MBUTTON) & PRESSED_MASK;
	case 2:
		return GetKeyState(VK_RBUTTON) & PRESSED_MASK;
	default:
		return false;
	}
}

bool glplatform_is_shift_pressed(struct glplatform_win *win)
{
	return GetKeyState(VK_SHIFT) & PRESSED_MASK;
}

bool glplatform_is_control_pressed(struct glplatform_win *win)
{
	return GetKeyState(VK_CONTROL) & PRESSED_MASK;
}

struct glplatform_context *glplatform_get_context_priv()
{
	return (struct glplatform_context *)TlsGetValue(g_context_tls);
}

bool glplatform_init()
{
	WNDCLASSEX wc;
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.style = CS_VREDRAW | CS_HREDRAW;
	wc.lpfnWndProc = PlatformWndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = 0;
	wc.lpszClassName = "glplatform";
	wc.hIcon = NULL;
	wc.hIconSm = NULL;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszMenuName = NULL;

	if (RegisterClassEx(&wc) == 0)
		return false;

	g_context_tls = TlsAlloc();

	if (g_context_tls == TLS_OUT_OF_INDEXES)
		return false;

	return true;
}

static bool in_client_area(struct glplatform_win *win)
{
	POINT pt;
	GetCursorPos(&pt);
	LRESULT ht = SendMessage(win->hwnd, WM_NCHITTEST, 0, MAKELONG(pt.x, pt.y));
	return (ht == HTCLIENT);
}

static void show_cursor_now(struct glplatform_win *win, bool state)
{
	if (state) {
		if (!g_cursor_count) {
			ShowCursor(TRUE);
			g_cursor_count++;
		}
	} else {
		if (g_cursor_count) {
			ShowCursor(FALSE);
			g_cursor_count--;
		}
	}
}

void glplatform_hide_cursor(struct glplatform_win *win)
{
	win->show_cursor = false;
	if (in_client_area(win)) {
		show_cursor_now(win, win->show_cursor);
	}
}

void glplatform_show_cursor(struct glplatform_win *win)
{
	win->show_cursor = true;
	if (in_client_area(win)) {
		show_cursor_now(win, win->show_cursor);
	}
}

void glplatform_shutdown()
{
	TlsFree(g_context_tls);
}

static LRESULT CALLBACK windows_event(struct glplatform_win *win, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK PlatformWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	struct glplatform_win *win;
	if (Msg == WM_CREATE) {
		CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
		win = (struct glplatform_win *)cs->lpCreateParams;
		SetWindowLongPtr(hWnd, -21/* GLW_USERDATA */, (LONG_PTR)win);
	}
	else {
		win = (struct glplatform_win *)GetWindowLongPtr(hWnd, -21 /* GLW_USERDATA */);
	}

	if (win) {
		return windows_event(win, hWnd, Msg, wParam, lParam);
	}
	else {
		return DefWindowProc(hWnd, Msg, wParam, lParam);;
	}
}

static LRESULT CALLBACK windows_event(struct glplatform_win *win, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	RECT cr;

	GetClientRect(hWnd, &cr);

	switch (Msg) {
	case WM_SETCURSOR: {
		WORD ht = LOWORD(lParam);
		if (ht == HTCLIENT) {
			show_cursor_now(win, win->show_cursor);
		} else {
			show_cursor_now(win, true);
		}
		return DefWindowProc(hWnd, Msg, wParam, lParam);
	} break;
	case WM_PAINT: {
		if (win->callbacks.on_expose)
			win->callbacks.on_expose(win);
		ValidateRect(hWnd, NULL);
	} break;
	case WM_SIZE: {
		int width = LOWORD(lParam);
		int height = HIWORD(lParam);
		win->width = width;
		win->height = height;
		if (win->callbacks.on_resize)
			win->callbacks.on_resize(win);
	} break;
	case WM_CREATE: {
		int width = cr.right;
		int height = cr.bottom;
		win->hdc = GetDC(hWnd);
		win->hwnd = hWnd;
		static PIXELFORMATDESCRIPTOR pfd;
		memset(&pfd, 0, sizeof(PIXELFORMATDESCRIPTOR));
		pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
		pfd.nVersion = 1;
		pfd.dwFlags = PFD_DRAW_TO_WINDOW |		//window drawing support
			PFD_SUPPORT_OPENGL |	//opengl support
			PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = win->fbformat.color_bits;
		pfd.cAlphaBits = win->fbformat.alpha_bits;
		pfd.cStencilBits = win->fbformat.stencil_bits;
		pfd.cAccumBits = win->fbformat.accum_bits;
		pfd.cDepthBits = win->fbformat.depth_bits;
		win->pixel_format = ChoosePixelFormat(win->hdc, &pfd);
		if (win->pixel_format == 0) {
			return -1;
		}
		if (SetPixelFormat(win->hdc, win->pixel_format, &pfd) == FALSE) {
			return -1;
		}
	} break;
	case WM_KEYDOWN: {
		WPARAM key = wParam;
		if (win->callbacks.on_key_down)
			win->callbacks.on_key_down(win, (int)key);
	} break;
	case WM_KEYUP: {
		WPARAM key = wParam;
		if (win->callbacks.on_key_up)
			win->callbacks.on_key_up(win, (int)key);
	} break;
	case WM_LBUTTONDOWN: {
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		if (win->callbacks.on_mouse_button_down)
			win->callbacks.on_mouse_button_down(win, 0, x, y);
	} break;
	case WM_LBUTTONUP: {
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		if (win->callbacks.on_mouse_button_up)
			win->callbacks.on_mouse_button_up(win, 0, x, y);
	} break;
	case WM_RBUTTONDOWN: {
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		if (win->callbacks.on_mouse_button_down)
			win->callbacks.on_mouse_button_down(win, 2, x, y);
	} break;
	case WM_RBUTTONUP: {
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		if (win->callbacks.on_mouse_button_up)
			win->callbacks.on_mouse_button_up(win, 2, x, y);
	} break;
	case WM_MOUSEMOVE: {
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		int lbutton_down = wParam & MK_LBUTTON;
		int rbutton_down = wParam & MK_RBUTTON;
		if (win->callbacks.on_mouse_move)
			win->callbacks.on_mouse_move(win, x, y);
	} break;
	case WM_MOUSEWHEEL: {
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		int delta = GET_WHEEL_DELTA_WPARAM(wParam);
		if (win->callbacks.on_mouse_wheel)
			win->callbacks.on_mouse_wheel(win, x, y, delta / WHEEL_DELTA);
	} break;
	case WM_CLOSE: {
		if (win->callbacks.on_destroy)
			win->callbacks.on_destroy(win);
	} break;
		/*
		case WM_DESTROY:{
		PostQuitMessage(0);
		} break;
		*/
	default: {
		return DefWindowProc(hWnd, Msg, wParam, lParam);
	} break;
	}
	return 0;
}

struct glplatform_win *glplatform_create_window(const char *title,
	const struct glplatform_win_callbacks *callbacks,
	const struct glplatform_fbformat *fbformat,
	int width, int height)
{
	static struct glplatform_fbformat default_fbformat = {
		.color_bits = 24,
		.alpha_bits = 8,
		.stencil_bits = 8,
		.depth_bits = 24,
		.accum_bits = 0
	};

	if (fbformat == NULL) {
		fbformat = &default_fbformat;
	}

	if (fbformat->color_bits % 3)
		return NULL;

	struct glplatform_win *win = (struct glplatform_win *) malloc(sizeof(struct glplatform_win));
	win->fbformat = *fbformat;
	win->callbacks = *callbacks;
	win->fullscreen = false;
	win->show_cursor = true;
	RECT wr = { 0, 0, width, height };
	AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

	HWND hwnd = CreateWindowEx(0, "glplatform",
		title,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		wr.right - wr.left,
		wr.bottom - wr.top,
		NULL,
		NULL,
		0,
		win);

	if (hwnd == NULL) {
		free(win);
		return NULL;
	}
	win->hwnd = hwnd;
	register_glplatform_win(win);
	if (win->callbacks.on_create)
		win->callbacks.on_create(win);

	return win;
}

//TODO
//void glplatform_set_win_transient_for(struct glplatform_win *win, intptr_t id)

//TODO
//void glplatform_set_win_type(struct glplatform_win *win, enum glplatform_win_types type)


void glplatform_make_current(struct glplatform_win *win, glplatform_gl_context_t ctx)
{
	struct glplatform_context *context = (struct glplatform_context*)ctx;

	TlsSetValue(g_context_tls, context);
	if (context)
		wglMakeCurrent(win->hdc, context->rc);
	else
		wglMakeCurrent(win->hdc, 0);
}

glplatform_gl_context_t glplatform_create_context(struct glplatform_win *win, int maj_ver, int min_ver)
{
	HGLRC temp = wglCreateContext(win->hdc);
	if (!temp)
		return false;
	wglMakeCurrent(win->hdc, temp);
	bool ret = glplatform_wgl_init(1, 0);
	wglDeleteContext(temp);

	if (!ret)
		return 0;

	if (!GLPLATFORM_WGL_ARB_create_context ||
		!GLPLATFORM_WGL_ARB_create_context_profile ||
		!GLPLATFORM_WGL_ARB_make_current_read) {

		return 0;
	}

	int attribList[] = {
		WGL_CONTEXT_MAJOR_VERSION_ARB, maj_ver,
		WGL_CONTEXT_MINOR_VERSION_ARB, min_ver,
		WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
		0
	};

	HGLRC rc = wglCreateContextAttribsARB(win->hdc, 0, attribList);

	if (!rc)
		return 0;
	struct glplatform_context *context = calloc(1, sizeof(struct glplatform_context));
	if (context)
		context->rc = rc;
	return (glplatform_gl_context_t)context;
}

void glplatform_fullscreen_win(struct glplatform_win *win, bool fullscreen)
{
	DWORD dwStyle = GetWindowLong(win->hwnd, GWL_STYLE);
	if (fullscreen && !win->fullscreen) {
		MONITORINFO mi = { sizeof(mi) };
		GetWindowPlacement(win->hwnd, &win->prev_placement);
		GetMonitorInfo(MonitorFromWindow(win->hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
		SetWindowLong(win->hwnd, GWL_STYLE, dwStyle & ~WS_OVERLAPPEDWINDOW);
		SetWindowPos(win->hwnd, HWND_TOP,
			mi.rcMonitor.left, mi.rcMonitor.top,
			mi.rcMonitor.right - mi.rcMonitor.left,
			mi.rcMonitor.bottom - mi.rcMonitor.top,
			SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	} else if (!fullscreen && win->fullscreen) {
		SetWindowLong(win->hwnd, GWL_STYLE, dwStyle | WS_OVERLAPPEDWINDOW);
		SetWindowPlacement(win->hwnd, &win->prev_placement);
		SetWindowPos(win->hwnd, NULL, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
			SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}
	win->fullscreen = fullscreen;
}

int glplatform_get_events(bool block)
{
	if (block) {
		if (!WaitMessage()) {
			return -1;
		}
		else {
			return 1;
		}
	} else {
		MSG Msg;
		return PeekMessage(&Msg, NULL, 0, 0, PM_NOREMOVE);
	}
}

bool glplatform_process_events()
{
	MSG Msg;
	while (PeekMessage(&Msg, NULL, 0, 0, PM_REMOVE)) {
		if (Msg.message == WM_QUIT) {
			return false;
		}
		TranslateMessage(&Msg);
		DispatchMessage(&Msg);
	}
	return g_glplatform_win_count > 0;
}

void glplatform_swap_buffers(struct glplatform_win *win)
{
	SwapBuffers(win->hdc);
}

void glplatform_destroy_window(struct glplatform_win *win)
{
	wglMakeCurrent(win->hdc, 0);
	DestroyWindow(win->hwnd);
	retire_glplatform_win(win);
	free(win);
}

void glplatform_show_window(struct glplatform_win *win)
{
	ShowWindow(win->hwnd, SW_SHOWNORMAL);
}

void glplatform_get_thread_state(struct glplatform_thread_state *state)
{
	state->read_dc = wglGetCurrentReadDCARB();
	state->draw_dc = wglGetCurrentDC();
	state->context = wglGetCurrentContext();
}

void glplatform_set_thread_state(const struct glplatform_thread_state *state)
{
	wglMakeContextCurrentARB(state->draw_dc, state->read_dc, state->context);
}
