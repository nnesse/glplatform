#include "glplatform.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>
#include <unistd.h>
#include "glplatform-glx.h"
#include "priv.h"

int glplatform_epoll_fd = -1;

static int g_x11_fd;
static int g_event_count;
static struct epoll_event g_events[100];
static Display *g_display;
static int g_screen;
static Atom g_delete_atom;
static int g_max_fd = 0;

static struct glplatform_win *g_win_list = NULL;
static int g_glplatform_win_count = 0;

struct fd_binding {
	struct glplatform_win *win;
	intptr_t user_data;
};

static struct fd_binding *g_fd_binding;

static pthread_key_t g_context_tls;

static Cursor g_empty_cursor;

static struct glplatform_win *find_glplatform_win(Window w)
{
	struct glplatform_win *win = g_win_list;
	while (win) {
		if (win->window == w)
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

	//Note: This is ineffient but we shouldn't be
	//destroying windows very often.
	int i;
	for (i = 0; i < g_max_fd; i++) {
		if (g_fd_binding[i].win == win) {
			glplatform_fd_unbind(i);
		}
	}
}

static void register_glplatform_win(struct glplatform_win *win)
{
	struct glplatform_win *test = find_glplatform_win(win->window);
	if (test)
		return;
	g_glplatform_win_count++;
	win->next = g_win_list;
	win->pprev = &g_win_list;
	g_win_list = win;
}

bool glplatform_is_button_pressed(struct glplatform_win *win, int button)
{
	switch (button)
	{
	case 1:
		return win->x_state_mask & Button1Mask;
		break;
	case 2:
		return win->x_state_mask & Button2Mask;
		break;
	case 3:
		return win->x_state_mask & Button3Mask;
		break;
	}
	return false;
}

bool glplatform_is_shift_pressed(struct glplatform_win *win)
{
	return win->x_state_mask & ShiftMask;
}

bool glplatform_is_control_pressed(struct glplatform_win *win)
{
	return win->x_state_mask & ControlMask;
}

static int handle_x_event(struct glplatform_win *win, XEvent *event)
{
	switch (event->type) {
	case KeymapNotify: {
		XRefreshKeyboardMapping(&event->xmapping);
	} break;
	case ConfigureNotify: {
		XConfigureEvent *configure_event = (XConfigureEvent *)event;
		if (win->width != configure_event->width || win->height != configure_event->height) {
			win->width = configure_event->width;
			win->height = configure_event->height;
			if (win->callbacks.on_resize)
				win->callbacks.on_resize(win);
		}
	} break;
	case KeyPress: {
		char buf[20];
		XKeyEvent *key_event = (XKeyEvent *)event;
		KeySym k;
		win->x_state_mask = key_event->state;
		XKeyEvent key_event_copy = *((XKeyEvent *)event);
		key_event_copy.state = 0;
		XLookupString(&key_event_copy, buf, 20, &k, NULL);
		if (win->callbacks.on_key_down)
			win->callbacks.on_key_down(win, toupper(k));
	} break;
	case KeyRelease: {
		char buf[20];
		XKeyEvent *key_event = (XKeyEvent *)event;
		KeySym k;
		win->x_state_mask = key_event->state;
		XKeyEvent key_event_copy = *((XKeyEvent *)event);
		key_event_copy.state = 0;
		XLookupString(&key_event_copy, buf, 20, &k, NULL);
		if (win->callbacks.on_key_up)
			win->callbacks.on_key_up(win, toupper(k));
	} break;
	case ButtonPress: {
		XButtonEvent *button_event = (XButtonEvent *)event;
		win->x_state_mask = button_event->state;
		switch (button_event->button) {
		case 1:
		case 2:
		case 3:
			if (win->callbacks.on_mouse_button_down)
				win->callbacks.on_mouse_button_down(win, button_event->button, button_event->x, button_event->y);
			break;
		case 4:
			if (win->callbacks.on_mouse_wheel)
				win->callbacks.on_mouse_wheel(win, button_event->x, button_event->y, -1);
			break;
		case 5:
			if (win->callbacks.on_mouse_wheel)
				win->callbacks.on_mouse_wheel(win, button_event->x, button_event->y, 1);
			break;
		}
	} break;
	case ButtonRelease: {
		XButtonEvent *button_event = (XButtonEvent *)event;
		win->x_state_mask = button_event->state;
		switch (button_event->button) {
		case 1:
		case 2:
		case 3:
			if (win->callbacks.on_mouse_button_up)
				win->callbacks.on_mouse_button_up(win, button_event->button, button_event->x, button_event->y);
			break;
		}
	} break;
	case MotionNotify: {
		XMotionEvent *motion_event = (XMotionEvent *)event;
		win->x_state_mask = motion_event->state;
		if (win->callbacks.on_mouse_move)
			win->callbacks.on_mouse_move(win,
				motion_event->x,
				motion_event->y);
	} break;
	case Expose: {
		XExposeEvent *expose_event = (XExposeEvent *)event;
		if (expose_event->count == 0) {
			if(win->callbacks.on_expose)
				win->callbacks.on_expose(win);
		}
	} break;
	case ClientMessage: {
		XClientMessageEvent *client_event = (XClientMessageEvent *)event;
		if (client_event->data.l[0] == g_delete_atom)
			if (win->callbacks.on_destroy)
				win->callbacks.on_destroy(win);
	} break;
	default:
		break;
	}
	if (win->callbacks.on_x_event)
		win->callbacks.on_x_event(win, event);
	return 0;
}

void glplatform_show_cursor(struct glplatform_win *win)
{
	XDefineCursor(g_display, win->window, None);
}

void glplatform_hide_cursor(struct glplatform_win *win)
{
	XDefineCursor(g_display, win->window, g_empty_cursor);
}

bool glplatform_init()
{
	int rc;
	g_event_count = 0;
	if (pthread_key_create(&g_context_tls, NULL))
		return false;

	glplatform_epoll_fd = epoll_create1(0);
	if (glplatform_epoll_fd == -1)
		goto error1;

	g_display = XOpenDisplay(NULL);
	if (g_display == NULL)
		goto error2;

	g_x11_fd = XConnectionNumber(g_display);
	glplatform_glx_init(1, 4);

	struct rlimit rl;
	rc = getrlimit(RLIMIT_NOFILE, &rl);
	if (rc)
		goto error3;

	g_max_fd = rl.rlim_max;
	g_fd_binding = (struct fd_binding *)calloc(rl.rlim_max, sizeof(struct fd_binding));
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = g_x11_fd;
	rc = epoll_ctl(glplatform_epoll_fd, EPOLL_CTL_ADD, g_x11_fd, &ev);
	if (rc == -1)
		goto error3;

	char empty = 0;
	Pixmap pixmap;
	XColor color;
	color.red = 0;
	color.green = 0;
	color.blue = 0;
        pixmap = XCreateBitmapFromData(g_display,
		DefaultRootWindow(g_display),
		&empty,
		1,
		1);
        if (!pixmap)
		goto error3;

	g_empty_cursor = XCreatePixmapCursor(g_display,
		pixmap, pixmap,
		&color, &color,
		0, 0);
	XFreePixmap(g_display, pixmap);

	if (g_empty_cursor == None)
		goto error3;

	g_screen = DefaultScreen(g_display);
	g_delete_atom = XInternAtom(g_display, "WM_DELETE_WINDOW", True);
	return true;
error3:
	XCloseDisplay(g_display);
	g_display = NULL;
error2:
	close(glplatform_epoll_fd);
	glplatform_epoll_fd = -1;
error1:
	pthread_key_delete(g_context_tls);
	g_context_tls = 0;
	return false;
}

void glplatform_shutdown()
{
	XCloseDisplay(g_display);
	close(glplatform_epoll_fd);
	pthread_key_delete(g_context_tls);
	g_display = NULL;
	g_context_tls = 0;
	glplatform_epoll_fd = -1;
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

	GLXFBConfig fb_config;
	Window window;
	GLXWindow glx_window;

	int fb_attributes[] = {
		/* attribute/value pairs */
		GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
		GLX_RENDER_TYPE, GLX_RGBA_BIT,
		GLX_DOUBLEBUFFER, 1,
		GLX_RED_SIZE, fbformat->color_bits / 3,
		GLX_GREEN_SIZE, fbformat->color_bits / 3,
		GLX_BLUE_SIZE, fbformat->color_bits / 3,
		GLX_ALPHA_SIZE, fbformat->alpha_bits,
		GLX_STENCIL_SIZE, fbformat->stencil_bits,
		GLX_DEPTH_SIZE, fbformat->depth_bits,
		GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,

		/* terminator */
		None
	};

	XVisualInfo *visual_info = NULL;
	int fb_count;
	GLXFBConfig *fb_config_a = NULL;

	fb_config_a = glXChooseFBConfig(g_display, g_screen, fb_attributes, &fb_count);

	if (fb_count == 0) {
		return NULL;
	}

	fb_config = fb_config_a[0];
	XFree(fb_config_a);

	visual_info = glXGetVisualFromFBConfig(g_display, fb_config);

	XSetWindowAttributes w_attr;
	Colormap colormap = XCreateColormap(g_display, RootWindow(g_display, g_screen), visual_info->visual, AllocNone);
	w_attr.background_pixel = 0;
	w_attr.border_pixel = 0;
	w_attr.colormap = colormap;
	w_attr.event_mask = KeymapStateMask |
		     KeyPressMask |
		     ExposureMask |
		     KeyReleaseMask |
		     ButtonPressMask |
		     ButtonReleaseMask |
		     PointerMotionMask |
		     StructureNotifyMask |
		     SubstructureNotifyMask;

	window = XCreateWindow(g_display,
			RootWindow(g_display, g_screen), /* parent */
			0, 0,    /* position */
			width, height, /* size */
			0, /* border width */
			visual_info->depth, /* depth */
			InputOutput, /* class */
			visual_info->visual,
			CWBackPixel | CWColormap | CWBorderPixel | CWEventMask, /* attribute valuemask */
			&w_attr);        /*attributes */

	XFree(visual_info);

	if (!window) {
		XFreeColormap(g_display, colormap);
		return NULL;
	}

	XStoreName(g_display, window, title);

	//Tell X that we want to process delete window client messages
	Atom wm_atoms[] = { g_delete_atom };
	XSetWMProtocols(g_display, window, wm_atoms, 1);

	glx_window = glXCreateWindow(g_display, fb_config, window, NULL);

	if (!glx_window) {
		XDestroyWindow(g_display, window);
		XFreeColormap(g_display, colormap);
		return NULL;
	}

	struct glplatform_win *win = (struct glplatform_win *) malloc(sizeof(struct glplatform_win));
	win->fbformat = *fbformat;
	win->callbacks = *callbacks;
	win->width = width;
	win->height = height;
	win->fb_config = fb_config;
	win->window = window;
	win->glx_window = glx_window;
	win->colormap = colormap;

	register_glplatform_win(win);
	if (win->callbacks.on_create)
		win->callbacks.on_create(win);
	return win;
}

void glplatform_set_win_transient_for(struct glplatform_win *win, intptr_t id)
{
	XSetTransientForHint(g_display, win->window, id);
}

void glplatform_set_win_type(struct glplatform_win *win, enum glplatform_win_types type)
{
	Atom type_atom = 0;
	switch (type) {
	case GLWIN_POPUP:
		type_atom = XInternAtom(g_display, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
		break;
	case GLWIN_NORMAL:
		type_atom = XInternAtom(g_display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
		break;
	case GLWIN_DIALOG:
		type_atom = XInternAtom(g_display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
		break;
	case GLWIN_TOOLBAR:
		type_atom = XInternAtom(g_display, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
		break;
	case GLWIN_UTILITY:
		type_atom = XInternAtom(g_display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
		break;
	default:
		return;
	}
	XChangeProperty(g_display,
		win->window,
		XInternAtom(g_display, "_NET_WM_WINDOW_TYPE", False),
		XA_ATOM,
		32,
		PropModeReplace,
		(const unsigned char *)&type_atom,
		1);
}

void glplatform_make_current(struct glplatform_win *win, glplatform_gl_context_t ctx)
{
	struct glplatform_context *context = (struct glplatform_context*)ctx;
	pthread_setspecific(g_context_tls, context);
	if (context)
		glXMakeContextCurrent(g_display, win->glx_window, win->glx_window, context->ctx);
	else
		glXMakeContextCurrent(g_display, win->glx_window, win->glx_window, NULL);
}

struct glplatform_context *glplatform_get_context_priv()
{
	return (struct glplatform_context *)pthread_getspecific(g_context_tls);
}

glplatform_gl_context_t glplatform_create_context(struct glplatform_win *win, int maj_ver, int min_ver)
{
	int attribList[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, maj_ver,
		GLX_CONTEXT_MINOR_VERSION_ARB, min_ver,
		GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
		0
	};
	GLXContext ctx = glXCreateContextAttribsARB(g_display, win->fb_config, 0, 1, attribList);
	struct glplatform_context *context = calloc(1, sizeof(struct glplatform_context));
	if (context)
		context->ctx = ctx;
	return (glplatform_gl_context_t)context;
}

void glplatform_fullscreen_win(struct glplatform_win *win, bool fullscreen)
{
	XWindowAttributes attr;
	bool mapped;

	win->fullscreen = fullscreen;

	Atom net_wm_state = XInternAtom(g_display, "_NET_WM_STATE", False);
	Atom net_wm_state_fullscreen = XInternAtom(g_display, "_NET_WM_STATE_FULLSCREEN", False);

	XGetWindowAttributes(g_display, win->window, &attr);
	mapped = (attr.map_state == IsUnmapped) ? false : true;

	if (mapped) {
		XEvent e;
		e.xany.type = ClientMessage;
		e.xclient.message_type = net_wm_state;
		e.xclient.format = 32;
		e.xclient.window = win->window;
		e.xclient.data.l[0] = fullscreen ? 1 : 0;
		e.xclient.data.l[1] = net_wm_state_fullscreen;
		e.xclient.data.l[3] = 0;
		XSendEvent(g_display,
				RootWindow(g_display, g_screen),
				0,
				SubstructureNotifyMask |
				SubstructureRedirectMask, &e);
		XFlush(g_display);

	} else {
		Atom atoms[2] = {
			net_wm_state_fullscreen,
			None
		};
		XChangeProperty(g_display,
			win->window,
			net_wm_state,
			XA_ATOM,
			32,
			PropModeReplace,
			(const unsigned char *)atoms,
			fullscreen ? 1 : 0);
	}
}

static Bool match_any_event(Display *display, XEvent *event, XPointer arg)
{
	return True;
}

int glplatform_get_events(bool block)
{
	int rc = 0;
	if (g_event_count < 100) {
		rc = epoll_wait(glplatform_epoll_fd, g_events + g_event_count, 100 - g_event_count, block ? -1 : 0);
		if (rc == -1) {
			fprintf(stderr, "glplatform_get_events(): epoll_wait() failed: %s", strerror(errno));
		} else {
			g_event_count += rc;
		}
	}
	return rc < 0 ? -1 : g_event_count;
}

bool glplatform_process_events()
{
	XEvent event;
	int i;
	for (i = 0; i < g_event_count; i++) {
		int fd = g_events[i].data.fd;
		struct glplatform_win *win = g_fd_binding[fd].win;
		if (win)
			if (win->callbacks.on_fd_event)
				win->callbacks.on_fd_event(win, fd, g_events[i].events, g_fd_binding[fd].user_data);
	}
	g_event_count = 0;

	while (XCheckIfEvent(g_display, &event, match_any_event, NULL) == True) {
		struct glplatform_win *win = find_glplatform_win(event.xany.window);
		if (win)
			handle_x_event(win, &event);
	}
	return g_glplatform_win_count > 0;
}

void glplatform_swap_buffers(struct glplatform_win *win)
{
	glXSwapBuffers(g_display, win->glx_window);
	XSync(g_display, 0);
}

void glplatform_destroy_window(struct glplatform_win *win)
{
	glXMakeContextCurrent(g_display, None, None, NULL);
	glXDestroyWindow(g_display, win->glx_window);
	XSync(g_display, 0);
	XDestroyWindow(g_display, win->window);
	XFreeColormap(g_display, win->colormap);
	retire_glplatform_win(win);
	free(win);
}

void glplatform_fd_bind(int fd, struct glplatform_win *win, intptr_t user_data)
{
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
	ev.data.fd = fd;
	epoll_ctl(glplatform_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
	g_fd_binding[fd].win = win;
	g_fd_binding[fd].user_data = user_data;
}

void glplatform_fd_unbind(int fd)
{
	epoll_ctl(glplatform_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
	g_fd_binding[fd].win = NULL;
	g_fd_binding[fd].user_data = 0;
}

void glplatform_show_window(struct glplatform_win *win)
{
	XMapRaised(g_display, win->window);
	XSync(g_display, 0);
}

void glplatform_get_thread_state(struct glplatform_thread_state *state)
{
	state->write_draw = glXGetCurrentDrawable();
	state->read_draw = glXGetCurrentDrawable();
	state->display = glXGetCurrentDisplay();
	state->context = glXGetCurrentContext();
}

void glplatform_set_thread_state(const struct glplatform_thread_state *state)
{
	glXMakeContextCurrent(state->display,
			state->write_draw,
			state->read_draw,
			state->context);
}
