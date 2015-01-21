#include "gltext_capi.h"

#include <math.h>

#include "edtaa3func.c"

#if !GLTEXT_USE_GLBINDIFY && !GLTEXT_USE_GLEW
#define GLTEXT_USE_GLEW 1
#endif

#if GLTEXT_USE_GLEW
#include "GL/glew.h"
#elif GLTEXT_USE_GLBINDIFY
#include "gl_3_3.h"
using namespace glbindify;
#endif

#include <ft2build.h>
#include FT_FREETYPE_H

enum vertex_attrib_locations {
	POS_LOC = 0,
	GLYPH_INDEX_LOC = 1,
	COLOR_LOC = 2,
};

//
// renderer
//
struct renderer
{
	//
	// Freetype state
	//
	FT_Library ft_library;

	//
	// OpenGL state
	//
	GLuint glsl_program;
	GLuint fragment_shader;
	GLuint vertex_passthrough_shader;
	GLuint geometry_shader;
	GLuint atlas_texture_name;
	GLuint gl_vertex_array;
	GLuint stream_vbo;
	GLuint texcoord_texture;
	GLuint texcoord_texture_buffer;
	GLuint glyph_size_texture;
	GLuint glyph_size_texture_buffer;
	int sampler_loc;
	int ambient_color_loc;
	int uvw_sampler_loc;
	int glyph_size_sampler_loc;
	int mvp_loc;
	struct gl_text__color ambient_color;
	int num_chars;

	uint8_t *atlas_buffer;
	bool *layer_loaded;
	bool initialized;
};

static const int texture_size = 128;

int gl_text__get_advance(const struct gl_text__glyph *prev, const struct gl_text__glyph *next)
{
	int ret = 0;
	if (prev) {
		int d = prev->advance_x;
		if (prev->font == next->font) {
			FT_Vector delta;
			const struct gl_text__font *font = prev->font;
			if (!FT_Get_Kerning(font->typeface,
					prev->typeface_index,
					next->typeface_index,
					FT_KERNING_DEFAULT,
					&delta)) {
				d += delta.x;
			}
		}
		ret += (d + 32)/64;
	}
	return ret;
}

struct gl_text__glyph_instance *gl_text__renderer__prepare_render(gl_text__renderer_t renderer_, int num_chars)
{
	struct renderer *inst = (struct renderer *)renderer_;
	if (!inst->initialized)
		return NULL;

	//Orphan previous buffer
	int buffer_size = sizeof(struct gl_text__glyph_instance) * num_chars;
	glBindBuffer(GL_ARRAY_BUFFER, inst->stream_vbo);
	glBufferData(GL_ARRAY_BUFFER,
			buffer_size,
			NULL,
			GL_STREAM_DRAW);
	//Copy in new data
	struct gl_text__glyph_instance * ret = (struct gl_text__glyph_instance *) glMapBufferRange(
		GL_ARRAY_BUFFER,
		0, buffer_size,
		GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	inst->num_chars = num_chars;
	return ret;
}

void gl_text__renderer__submit_render(gl_text__renderer_t renderer, const float *mvp)
{
	struct renderer *inst = (struct renderer *)renderer;
	glUnmapBuffer(GL_ARRAY_BUFFER);
	glUseProgram(inst->glsl_program);
	glBindVertexArray(inst->gl_vertex_array);
	glUniformMatrix4fv(inst->mvp_loc, 1, GL_FALSE, mvp);
	glUniform4fv(inst->ambient_color_loc, 1, (GLfloat *)&inst->ambient_color);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, inst->atlas_texture_name);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_BUFFER, inst->texcoord_texture);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_BUFFER, inst->glyph_size_texture);
	glDrawArrays(GL_POINTS, 0, inst->num_chars);
}

void gl_text__renderer__set_ambient_color(gl_text__renderer_t renderer, struct gl_text__color *color)
{
	struct renderer *inst = (struct renderer *)renderer;
	inst->ambient_color = *color;
}

gl_text__renderer_t gl_text__renderer__create()
{
	struct renderer *inst = malloc(sizeof(struct renderer));
	memset(inst, 0, sizeof(*inst));
	inst->ft_library = NULL;
	inst->glsl_program = 0;
	inst->fragment_shader = 0;
	inst->vertex_passthrough_shader = 0;
	inst->geometry_shader = 0;
	inst->atlas_texture_name = 0;
	inst->ambient_color.r = 1;
	inst->ambient_color.g = 1;
	inst->ambient_color.b = 1;
	inst->ambient_color.a = 1;
	inst->initialized = false;
	FT_Init_FreeType(&inst->ft_library);
	return inst;
}

void gl_text__renderer__free(gl_text__renderer_t renderer)
{
	struct renderer *inst = (struct renderer *)renderer;
	if (inst->fragment_shader)
		glDeleteShader(inst->fragment_shader);
	if (inst->geometry_shader)
		glDeleteShader(inst->geometry_shader);
	if (inst->vertex_passthrough_shader)
		glDeleteShader(inst->vertex_passthrough_shader);
	if (inst->glsl_program)
		glDeleteProgram(inst->glsl_program);
	if (inst->ft_library)
		FT_Done_FreeType(inst->ft_library);
	if (inst->atlas_texture_name)
		glDeleteTextures(1, &inst->atlas_texture_name);
	free(inst);
}

gl_text__typeface_t gl_text__renderer__get_typeface(gl_text__renderer_t renderer, const char *path)
{
	struct renderer *inst = (struct renderer *)renderer;
	FT_Face face;
	int error = FT_New_Face(inst->ft_library, path, 0, &face);

	if (error) {
		return NULL;
	}
	return face;
}

bool init_program(struct renderer *inst)
{
	const char *vertex_passthrough_shader_text =
		"#version 330\n"
		"layout (location = 0) in vec2 pos_v;\n"
		"layout (location = 1) in int glyph_index_v;\n"
		"layout (location = 2) in vec4 color_v;\n"
		"out vec2 pos;\n"
		"out int glyph_index;\n"
		"out vec4 color;\n"
		"void main()\n"
		"{\n"
				"pos = pos_v;\n"
				"color = color_v;\n"
				"glyph_index = glyph_index_v;\n"
				"gl_Position = vec4(0, 0, 1, 1);\n"
		"}\n";

	const char *geometry_shader_text =
		"#version 330\n"
		"layout(points) in;\n"
		"layout(triangle_strip, max_vertices=4) out;\n"
		"in vec2 pos[1];\n"
		"in vec4 color[1];\n"
		"in int glyph_index[1];\n"
		"uniform mat4 mvp;\n"
		"uniform usamplerBuffer glyph_size_sampler;\n"
		"uniform usamplerBuffer uvw_sampler;\n"
		"out vec3 texcoord_f;\n"
		"out vec4 color_f;\n"
		"\n"
		"void genVertex(vec2 corner, vec2 size, vec3 texcoord)\n"
		"{\n"
			"gl_Position = mvp * vec4((pos[0] - vec2(8, 8) +  (corner * size)), 0, 1);\n"
			"texcoord_f = texcoord + vec3(corner * size, 0);\n"
			"color_f = color[0];\n"
			"EmitVertex();\n"
		"}\n"
		"\n"
		"void main()\n"
		"{\n"
			"if (glyph_index[0] >= 0) {\n"
				"vec2 size = vec2(texelFetch(glyph_size_sampler, glyph_index[0]).rg);\n"
				"size += vec2(16);\n"
				"vec3 texcoord = vec3(texelFetch(uvw_sampler, glyph_index[0]).rgb);\n"
				"texcoord -= vec3(8, 8, 0);\n"
				"genVertex(vec2(0,0), size, texcoord);\n"
				"genVertex(vec2(1,0), size ,texcoord);\n"
				"genVertex(vec2(0,1), size, texcoord);\n"
				"genVertex(vec2(1,1), size, texcoord);\n"
				"EndPrimitive();\n"
			"}\n"
		"}\n";

	const char *fragment_shader_text =
		"#version 330\n"
		"uniform sampler2DArray sampler;\n"
		"uniform vec4 ambient_color;\n"
		"in vec3 texcoord_f;\n"
		"in vec4 color_f;\n"
		"out vec4 frag_color;\n"
		"void main()\n"
		"{\n"
			"ivec3 tex_size = textureSize(sampler, 0);\n"
			"float D = texture(sampler, vec3(vec2(texcoord_f.xy) / tex_size.xy, texcoord_f.z)).r;\n"
			"D = (D - 0.49) * 16.0;\n"
			"float aastep = length(vec2(dFdx(D), dFdy(D)));\n"
			"float texel = smoothstep(-aastep, aastep, D);\n"
			"frag_color = ambient_color * color_f * vec4(texel);\n"
		"}\n";

	GLint success;
	glGenVertexArrays(1, &inst->gl_vertex_array);
	glBindVertexArray(inst->gl_vertex_array);

	inst->geometry_shader = glCreateShader(GL_GEOMETRY_SHADER);
	glShaderSource(inst->geometry_shader, 1, (const char **)&geometry_shader_text, NULL);
	glCompileShader(inst->geometry_shader);
	glGetShaderiv(inst->geometry_shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		char info_log[1000];
		glGetShaderInfoLog(inst->geometry_shader, sizeof(info_log), NULL, info_log);
		printf("renderer: Geometry shader compile failed\n%s", info_log);
		glDeleteShader(inst->geometry_shader);
		inst->geometry_shader = 0;
		return false;
	}

	inst->fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(inst->fragment_shader, 1, (const char **)&fragment_shader_text, NULL);
	glCompileShader(inst->fragment_shader);
	glGetShaderiv(inst->fragment_shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		char info_log[1000];
		glGetShaderInfoLog(inst->fragment_shader, sizeof(info_log), NULL, info_log);
		printf("renderer: Fragment shader compile failed\n%s", info_log);
		glDeleteShader(inst->fragment_shader);
		inst->fragment_shader = 0;
		glDeleteShader(inst->geometry_shader);
		inst->geometry_shader = 0;
		return false;
	}

	inst->vertex_passthrough_shader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(inst->vertex_passthrough_shader, 1, (const char **)&vertex_passthrough_shader_text, NULL);
	glCompileShader(inst->vertex_passthrough_shader);
	glGetShaderiv(inst->vertex_passthrough_shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		char info_log[1000];
		glGetShaderInfoLog(inst->vertex_passthrough_shader, sizeof(info_log), NULL, info_log);
		printf("renderer: Vertex shader compile failed\n%s", info_log);
		glDeleteShader(inst->fragment_shader);
		inst->fragment_shader = 0;
		glDeleteShader(inst->vertex_passthrough_shader);
		inst->vertex_passthrough_shader = 0;
		glDeleteShader(inst->geometry_shader);
		inst->geometry_shader = 0;
		return false;
	}

	inst->glsl_program = glCreateProgram();
	glAttachShader(inst->glsl_program, inst->vertex_passthrough_shader);
	glAttachShader(inst->glsl_program, inst->geometry_shader);
	glAttachShader(inst->glsl_program, inst->fragment_shader);
	glLinkProgram(inst->glsl_program);
	glGetProgramiv(inst->glsl_program, GL_LINK_STATUS, &success);
	if (!success) {
		char info_log[1000];
		glGetProgramInfoLog(inst->glsl_program, sizeof(info_log), NULL, info_log);
		printf("renderer: Program link failed\n%s", info_log);
		glDeleteShader(inst->fragment_shader);
		inst->fragment_shader = 0;
		glDeleteShader(inst->vertex_passthrough_shader);
		inst->vertex_passthrough_shader = 0;
		glDeleteProgram(inst->glsl_program);
		inst->glsl_program = 0;
		return false;
	}

	//
	// Common setup for GL 3.x and GL 4.x
	//
	glEnableVertexAttribArray(GLYPH_INDEX_LOC);
	glEnableVertexAttribArray(POS_LOC);
	glEnableVertexAttribArray(COLOR_LOC);

	glGenBuffers(1, &inst->stream_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, inst->stream_vbo);

	glVertexAttribPointer(POS_LOC,
		2,
		GL_FLOAT,
		GL_FALSE,
		sizeof(struct gl_text__glyph_instance),
		(void *)offsetof(struct gl_text__glyph_instance, pos));

	glVertexAttribPointer(COLOR_LOC,
		4,
		GL_FLOAT,
		GL_FALSE,
		sizeof(struct gl_text__glyph_instance),
		(void *)offsetof(struct gl_text__glyph_instance, color));

	glVertexAttribIPointer(GLYPH_INDEX_LOC,
		1,
		GL_UNSIGNED_INT,
		sizeof(struct gl_text__glyph_instance),
		(void *)offsetof(struct gl_text__glyph_instance, atlas_index));

	//Cache uniform locations
	inst->mvp_loc = glGetUniformLocation(inst->glsl_program, "mvp");
	inst->sampler_loc = glGetUniformLocation(inst->glsl_program, "sampler");
	inst->uvw_sampler_loc = glGetUniformLocation(inst->glsl_program, "uvw_sampler");
	inst->ambient_color_loc = glGetUniformLocation(inst->glsl_program, "ambient_color");
	inst->glyph_size_sampler_loc = glGetUniformLocation(inst->glsl_program, "glyph_size_sampler");

	glUseProgram(inst->glsl_program);
	glUniform1i(inst->sampler_loc, 0);
	glUniform1i(inst->uvw_sampler_loc, 1);
	glUniform1i(inst->glyph_size_sampler_loc, 2);
	glUniform4fv(inst->ambient_color_loc, 1, (GLfloat *)&inst->ambient_color);
	return true;
}

bool gl_text__renderer__initialize(gl_text__renderer_t renderer,
	const struct gl_text__font_desc *font_descriptions,
	int count,
	const struct gl_text__font **fonts)
{
	struct renderer *inst = (struct renderer *)renderer;
	if (inst->initialized)
		return false; //You can't reinitialize the renderer

	if (!inst->ft_library)
		return false;

	//Need at least one font
	if (!count)
		return false;

	//Create GLSL program
	if (!init_program(inst))
		return false;

	//Initialize glyph bitmaps
	int total_glyphs = 0;
	int i;
	for (i = 0; i < count; i++) {
		struct gl_text__font *f = (struct gl_text__font *)malloc(sizeof(struct gl_text__font));
		const struct gl_text__font_desc *font_desc = font_descriptions + i;
		FT_Face typeface = (FT_Face) font_desc->typeface;
		int width = font_desc->width;
		int height = font_desc->height;

		if (!typeface)
			return false;

		int max_char = 0;
		for (const char *c = font_desc->charset; *c; c++)
			if (*c > max_char)
				max_char = *c;

		if ('\n' > max_char)
			max_char = '\n';

		f->glyph_array = malloc((max_char + 1) * sizeof(struct gl_text__glyph));
		f->width = width;
		f->height = height;
		f->typeface = typeface;
		f->family = font_desc->family;
		f->style = font_desc->style;
		f->renderer = renderer;
		FT_Set_Pixel_Sizes(typeface, width, height);

		for (const char *c = font_desc->charset; *c; c++) {
			struct gl_text__glyph *g = f->glyph_array + (*c);
			int index = FT_Get_Char_Index(typeface, *c);
			FT_Load_Glyph(typeface, index, 0/* flags */);
			FT_Render_Glyph(typeface->glyph, FT_RENDER_MODE_NORMAL);
			g->c = *c;
			g->bitmap_pitch = typeface->glyph->bitmap.pitch;
			g->bitmap_width = typeface->glyph->bitmap.width;
			g->bitmap_height = typeface->glyph->bitmap.rows;
			size_t image_size = g->bitmap_pitch * g->bitmap_height;
			g->bitmap_bits = (uint8_t *)malloc(image_size);
			memcpy(g->bitmap_bits, typeface->glyph->bitmap.buffer, image_size);
			g->left = typeface->glyph->bitmap_left;
			g->top = typeface->glyph->bitmap_top;
			g->advance_x = typeface->glyph->advance.x;
			g->advance_y = typeface->glyph->advance.y;
			g->font = f;
			g->typeface_index = index;
			total_glyphs++;
		}
		f->glyph_array['\n'].c = '\n';
		f->glyph_array['\n'].font = f;
		fonts[i] = f;
	}

	//
	// Position the glyphs in the altas's uvw space
	//
	// TODO: Use a priority queue to assign glyph positions
	//
	int u = 16;
	int v = 16;
	int w = 0;
	int height = 0;
	bool first = true;
	for (i = 0; i < count; i++) {
		const struct gl_text__font *f = fonts[i];
		const struct gl_text__font_desc *font_desc = font_descriptions + i;
		for (const char *c = font_desc->charset; *c; c++) {
			struct gl_text__glyph *g = f->glyph_array + (*c);
			if (first) {
				first = false;
				height = g->bitmap_height;
			}
			if ((u + g->bitmap_width + 16) > texture_size) {
				u = 16;
				v += height + 16;
				if ((v + g->bitmap_height + 16) > texture_size) {
					u = 16;
					v = 16;
					w++;
				}
				height = g->bitmap_height;
			}
			g->u = u;
			g->v = v;
			g->w = w;
			u += g->bitmap_width + 16;
		}
	}
	int tex_array_size = w + 1;

	//
	// Blit the glyph bitmaps into a CPU texture
	//
	// TODO: Convert glyphs to signed distance fields here rather than applying the transformation
	// on the entire atlas
	//
	inst->atlas_buffer = (uint8_t *)malloc(tex_array_size * texture_size * texture_size);
	inst->layer_loaded = (bool *)malloc(tex_array_size * sizeof(bool));
	int16_t *texcoord_array = (int16_t *)malloc(total_glyphs * sizeof(int16_t) * 4);
	int16_t *glyph_size_array = (int16_t *)malloc(total_glyphs * sizeof(int16_t) * 2);
	int16_t *texcoord_ptr = texcoord_array;
	int16_t *glyph_size_ptr = glyph_size_array;
	int j = 0;
	for (i = 0; i < count; i++) {
		const struct gl_text__font *f = fonts[i];
		const struct gl_text__font_desc *font_desc = font_descriptions + i;
		for (const char *c = font_desc->charset; *c; c++) {
			struct gl_text__glyph *g = f->glyph_array + (*c);
			uint8_t *dest = inst->atlas_buffer + g->u + (g->v * texture_size) + (g->w * texture_size * texture_size);
			uint8_t *source = g->bitmap_bits;
			for (int i = 0; i < g->bitmap_height; i++) {
				memcpy(dest, source, g->bitmap_width);
				dest += texture_size;
				source += g->bitmap_pitch;
			}
			g->atlas_index = j++;
			*(texcoord_ptr++) = g->u;
			*(texcoord_ptr++) = g->v;
			*(texcoord_ptr++) = g->w;
			*(texcoord_ptr++) = 0;
			*(glyph_size_ptr++) = (int16_t)g->bitmap_width;
			*(glyph_size_ptr++) = (int16_t)g->bitmap_height;
		}
	}

	//
	//Setup texcoord texture buffer
	//
	glGenBuffers(1, &inst->texcoord_texture_buffer);
	glBindBuffer(GL_TEXTURE_BUFFER, inst->texcoord_texture_buffer);
	glBufferData(GL_TEXTURE_BUFFER, total_glyphs * 4 * 2, texcoord_array, GL_STATIC_DRAW);
	glGenTextures(1, &inst->texcoord_texture);
	glBindTexture(GL_TEXTURE_BUFFER, inst->texcoord_texture);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA16I, inst->texcoord_texture_buffer);
	free(texcoord_array);

	//
	//Setup glyph size texture buffer
	//
	glGenBuffers(1, &inst->glyph_size_texture_buffer);
	glBindBuffer(GL_TEXTURE_BUFFER, inst->glyph_size_texture_buffer);
	glBufferData(GL_TEXTURE_BUFFER, total_glyphs * 2 * 2, glyph_size_array, GL_STATIC_DRAW);
	glGenTextures(1, &inst->glyph_size_texture);
	glBindTexture(GL_TEXTURE_BUFFER, inst->glyph_size_texture);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_RG16I, inst->glyph_size_texture_buffer);
	free(glyph_size_array);

	//
	// Allocate and altas texture and setup texture filtering
	//
	glGenTextures(1, &inst->atlas_texture_name);
	glBindTexture(GL_TEXTURE_2D_ARRAY, inst->atlas_texture_name);
	glTexImage3D(GL_TEXTURE_2D_ARRAY,
		0, /* layer */
		GL_R8,
		texture_size, texture_size, tex_array_size /* uvw size */,
		0, /* border */
		GL_RED,
		GL_UNSIGNED_BYTE,
		NULL);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	inst->initialized = true;
	return inst->initialized;
}
