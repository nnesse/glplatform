#include "gltext.h"

#include <math.h>

#include "edtaa3func.c"

#define GLPLATFORM_GL_VERSION 33
#include "glplatform-glcore.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <string.h>

enum vertex_attrib_locations {
	POS_LOC = 0,
	GLYPH_INDEX_LOC = 1
};

//
// font
//
struct gltext_font {
	int size;
	struct gltext_glyph *glyph_array;
	int total_glyphs;
	int8_t *glyph_metric_array;
	uint8_t *atlas_buffer;
	int pot_size;
	GLuint atlas_texture;
	GLuint glyph_metric_texture;
	GLuint glyph_metric_texture_buffer;
	int16_t *kerning_table;
	int max_char;
};

const struct gltext_glyph *gltext_get_glyph(gltext_font_t font, char32_t c)
{
	struct gltext_glyph *g;
	if (c > font->max_char) {
		return NULL;
	}
        g = font->glyph_array + c;
	if (!g->valid) {
		return NULL;
	}
	return g;
}

//
// gltext_renderer
//
struct gltext_renderer
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
	GLuint vertex_shader;
	GLuint geometry_shader;
	GLuint gl_vertex_array;
	GLuint stream_vbo;
	int sampler_loc;
	int color_loc;
	int glyph_metric_sampler_loc;
	int mvp_loc;
	int num_chars;
};

static bool init_renderer(struct gltext_renderer *inst);

#include "glplatform_priv.h"
static struct gltext_renderer *get_renderer()
{
	struct glplatform_context *context = glplatform_get_context_priv();
	if (!context)
		return NULL;
	if (!context->text_renderer) {
		struct gltext_renderer *inst = calloc(1, sizeof(struct gltext_renderer));
		if (!inst)
			return NULL;
		if (!init_renderer(inst)) {
			free(inst);
			return NULL;
		}
		context->text_renderer = inst;
	}
	return context->text_renderer;
}

float gltext_get_advance(const struct gltext_glyph *prev, const struct gltext_glyph *next)
{
	float ret = 0;
	if (next && prev) {
		int d = prev->advance_x;
		if (prev->font == next->font) {
			struct gltext_font *font = prev->font;
			d += *(font->kerning_table + (prev->w * font->total_glyphs) + next->w);
		}
		ret += (float)d/64.0f;
	} else if (prev && !next) {
		ret = prev->left + prev->bitmap_width;
	}
	return ret;
}

struct gltext_glyph_instance *gltext_prepare_render(gltext_font_t font, int num_chars)
{
	struct gltext_renderer *inst = get_renderer();

	if (!inst)
		return NULL;

	if (!font->atlas_texture)
		gltext_font_create_texture(font);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, font->atlas_texture);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_BUFFER, font->glyph_metric_texture);

	//Orphan previous buffer
	int buffer_size = sizeof(struct gltext_glyph_instance) * num_chars;
	glBindBuffer(GL_ARRAY_BUFFER, inst->stream_vbo);
	glBufferData(GL_ARRAY_BUFFER,
			buffer_size,
			NULL,
			GL_STREAM_DRAW);

	//Copy in new data
	struct gltext_glyph_instance * ret = (struct gltext_glyph_instance *) glMapBufferRange(
		GL_ARRAY_BUFFER,
		0, buffer_size,
		GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	inst->num_chars = num_chars;
	return ret;
}

void gltext_submit_render(const struct gltext_color *color, int num_chars, const float *mvp)
{
	struct gltext_renderer *inst = get_renderer();
	if (!inst)
		return;
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glUnmapBuffer(GL_ARRAY_BUFFER);
	glUseProgram(inst->glsl_program);
	glBindVertexArray(inst->gl_vertex_array);
	glUniformMatrix4fv(inst->mvp_loc, 1, GL_FALSE, mvp);
	glUniform4fv(inst->color_loc, 1, (GLfloat *)color);
	glDrawArrays(GL_POINTS, 0, num_chars);
}

void deinit_renderer(struct gltext_renderer *inst)
{
	glDeleteShader(inst->fragment_shader);
	glDeleteShader(inst->geometry_shader);
	glDeleteShader(inst->vertex_shader);
	glDeleteProgram(inst->glsl_program);
	FT_Done_FreeType(inst->ft_library);
}

gltext_typeface_t gltext_get_typeface(const char *path)
{
	FT_Face face;
	struct gltext_renderer *inst = get_renderer();
	if (!inst)
		return NULL;

	int error = FT_New_Face(inst->ft_library, path, 0, &face);
	if (error) {
		return NULL;
	}
	return face;
}

static bool init_renderer(struct gltext_renderer *inst)
{
	FT_Init_FreeType(&inst->ft_library);
	if (!inst->ft_library)
		goto error0;
	const char *vertex_shader_text =
		"#version 330\n"
		"layout (location = 0) in vec2 pos_v;\n"
		"layout (location = 1) in int glyph_index_v;\n"
		"layout (location = 2) in vec4 color_v;\n"
		"out vec2 pos;\n"
		"out int glyph_index;\n"
		"void main()\n"
		"{\n"
			"pos = pos_v;\n"
			"glyph_index = glyph_index_v;\n"
			"gl_Position = vec4(0, 0, 1, 1);\n"
		"}\n";

	const char *geometry_shader_text =
		"#version 330\n"
		"layout(points) in;\n"
		"layout(triangle_strip, max_vertices=4) out;\n"
		"in vec2 pos[1];\n"
		"in int glyph_index[1];\n"
		"uniform mat4 mvp;\n"
		"uniform isamplerBuffer glyph_metric_sampler;\n"
		"out vec3 texcoord_f;\n"
		"\n"
		"void genVertex(vec2 ul, vec2 corner, vec2 size, vec3 texcoord)\n"
		"{\n"
			"gl_Position = mvp * vec4((ul - vec2(8, 8) + (corner * size)), 0, 1);\n"
			"texcoord_f = texcoord + vec3(corner * size, 0);\n"
			"EmitVertex();\n"
		"}\n"
		"\n"
		"void main()\n"
		"{\n"
			"if (glyph_index[0] >= 0) {\n"
				"vec4 glyph_metrics = texelFetch(glyph_metric_sampler, glyph_index[0]);\n"
				"vec2 size = glyph_metrics.xy + vec2(16,16);\n"
				"vec2 ul = pos[0] + vec2(glyph_metrics.z, -glyph_metrics.w);\n"
				"vec3 texcoord = vec3(0, 0, glyph_index[0]);\n"
				"genVertex(ul, vec2(0, 0), size, texcoord);\n"
				"genVertex(ul, vec2(1, 0), size ,texcoord);\n"
				"genVertex(ul, vec2(0, 1), size, texcoord);\n"
				"genVertex(ul, vec2(1, 1), size, texcoord);\n"
				"EndPrimitive();\n"
			"}\n"
		"}\n";

	const char *fragment_shader_text =
		"#version 330\n"
		"uniform sampler2DArray sampler;\n"
		"uniform vec4 color;\n"
		"in vec3 texcoord_f;\n"
		"out vec4 frag_color;\n"
		"void main()\n"
		"{\n"
			"ivec3 tex_size = textureSize(sampler, 0);\n"
			"float D = texture(sampler, vec3(texcoord_f.xy/tex_size.xy, texcoord_f.z)).r - 0.50;\n"
			"float aastep = length(vec2(dFdx(D), dFdy(D)));\n"
			"float texel = smoothstep(-aastep, aastep, D);\n"
			"frag_color = color * vec4(texel);\n"
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
		goto error2;
	}

	inst->fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(inst->fragment_shader, 1, (const char **)&fragment_shader_text, NULL);
	glCompileShader(inst->fragment_shader);
	glGetShaderiv(inst->fragment_shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		char info_log[1000];
		glGetShaderInfoLog(inst->fragment_shader, sizeof(info_log), NULL, info_log);
		printf("renderer: Fragment shader compile failed\n%s", info_log);
		goto error3;
	}

	inst->vertex_shader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(inst->vertex_shader, 1, (const char **)&vertex_shader_text, NULL);
	glCompileShader(inst->vertex_shader);
	glGetShaderiv(inst->vertex_shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		char info_log[1000];
		glGetShaderInfoLog(inst->vertex_shader, sizeof(info_log), NULL, info_log);
		printf("renderer: Vertex shader compile failed\n%s", info_log);
		goto error4;
	}

	inst->glsl_program = glCreateProgram();
	glAttachShader(inst->glsl_program, inst->vertex_shader);
	glAttachShader(inst->glsl_program, inst->geometry_shader);
	glAttachShader(inst->glsl_program, inst->fragment_shader);
	glLinkProgram(inst->glsl_program);
	glGetProgramiv(inst->glsl_program, GL_LINK_STATUS, &success);
	if (!success) {
		char info_log[1000];
		glGetProgramInfoLog(inst->glsl_program, sizeof(info_log), NULL, info_log);
		printf("renderer: Program link failed\n%s", info_log);
		goto error4;
	}

	glEnableVertexAttribArray(GLYPH_INDEX_LOC);
	glEnableVertexAttribArray(POS_LOC);

	glGenBuffers(1, &inst->stream_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, inst->stream_vbo);

	glVertexAttribPointer(POS_LOC,
		2,
		GL_FLOAT,
		GL_FALSE,
		sizeof(struct gltext_glyph_instance),
		(void *)offsetof(struct gltext_glyph_instance, pos));

	glVertexAttribIPointer(GLYPH_INDEX_LOC,
		1,
		GL_UNSIGNED_INT,
		sizeof(struct gltext_glyph_instance),
		(void *)offsetof(struct gltext_glyph_instance, w));

	//Cache uniform locations
	inst->mvp_loc = glGetUniformLocation(inst->glsl_program, "mvp");
	inst->sampler_loc = glGetUniformLocation(inst->glsl_program, "sampler");
	inst->color_loc = glGetUniformLocation(inst->glsl_program, "color");
	inst->glyph_metric_sampler_loc = glGetUniformLocation(inst->glsl_program, "glyph_metric_sampler");

	glUseProgram(inst->glsl_program);
	glUniform1i(inst->sampler_loc, 0);
	glUniform1i(inst->glyph_metric_sampler_loc, 1);
	return true;
error4:
	glDeleteProgram(inst->glsl_program);
	inst->glsl_program = 0;
error3:
	glDeleteShader(inst->vertex_shader);
	inst->vertex_shader = 0;
error2:
	glDeleteShader(inst->fragment_shader);
	inst->fragment_shader = 0;
error1:
	glDeleteShader(inst->geometry_shader);
	inst->geometry_shader = 0;
	glDeleteVertexArrays(1, &inst->gl_vertex_array);
	inst->gl_vertex_array = 0;
	FT_Done_FreeType(inst->ft_library);
error0:
	return false;
}

void gltext_font_create_texture(gltext_font_t f)
{
	if (!f->atlas_texture) {
		//
		//Setup glyph size texture buffer
		//
		glGenBuffers(1, &f->glyph_metric_texture_buffer);
		glBindBuffer(GL_TEXTURE_BUFFER, f->glyph_metric_texture_buffer);
		glBufferData(GL_TEXTURE_BUFFER, f->total_glyphs * 4, f->glyph_metric_array, GL_STATIC_DRAW);
		glGenTextures(1, &f->glyph_metric_texture);
		glBindTexture(GL_TEXTURE_BUFFER, f->glyph_metric_texture);
		glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8I, f->glyph_metric_texture_buffer);

		//
		// Allocate and altas texture and setup texture filtering
		//
		glGenTextures(1, &f->atlas_texture);
		glBindTexture(GL_TEXTURE_2D_ARRAY, f->atlas_texture);
		glTexImage3D(GL_TEXTURE_2D_ARRAY,
			0, /* layer */
			GL_R8,
			f->pot_size, f->pot_size, f->total_glyphs /* uvw size */,
			0, /* border */
			GL_RED,
			GL_UNSIGNED_BYTE,
			f->atlas_buffer);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
}

void gltext_font_destroy_texture(gltext_font_t font)
{
	if (font->atlas_texture) {
		glDeleteBuffers(1, &font->glyph_metric_texture_buffer);
		glDeleteTextures(1, &font->glyph_metric_texture);
		glDeleteTextures(1, &font->atlas_texture);
		font->glyph_metric_texture_buffer = 0;
		font->glyph_metric_texture = 0;
		font->atlas_buffer = 0;
	}
}

gltext_font_t gltext_font_create(const char32_t *charset,
	gltext_typeface_t typeface_,
	int size)
{
	FT_Face typeface = (FT_Face) typeface_;
	struct gltext_renderer *inst = get_renderer();
	if (!inst)
		return 0;

	if (!typeface)
		return 0;

	if (FT_Select_Charmap(typeface, ft_encoding_unicode))
		return 0;

	FT_Set_Pixel_Sizes(typeface, size, size);

	struct gltext_font *f = (struct gltext_font *)calloc(1, sizeof(struct gltext_font));

	f->size = size;

	int charset_len = 0;
        while (charset[charset_len])
		charset_len++;
	f->total_glyphs = charset_len;

	int max_char = 0;
	for (int i = 0; i < f->total_glyphs; i++) {
		uint32_t c = charset[i];
		if (c > max_char)
			max_char = c;
	}

	if ('\n' > max_char)
		max_char = '\n';
	f->max_char = max_char;
	f->glyph_array = calloc((f->max_char + 1), sizeof(struct gltext_glyph));
	f->kerning_table = (int16_t *)calloc(f->total_glyphs * f->total_glyphs, sizeof(int16_t));

	int max_dim = 0;
	int w = 0;

	for (int i = 0; i < f->total_glyphs; i++) {
		uint32_t c = charset[i];
		struct gltext_glyph *g = f->glyph_array + c;
		g->w = w++;
		int index = FT_Get_Char_Index(typeface, c);
		if (!index)
			continue;
		FT_Load_Glyph(typeface, index, 0/* flags */);
		FT_Render_Glyph(typeface->glyph, FT_RENDER_MODE_NORMAL);
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
		g->valid = true;
		max_dim = g->bitmap_width > max_dim ? g->bitmap_width : max_dim;
		max_dim = g->bitmap_height > max_dim ? g->bitmap_height : max_dim;
	}

	for (int i = 0; i < f->total_glyphs; i++) {
		uint32_t cprev = charset[i];
		struct gltext_glyph *prev = f->glyph_array + cprev;
		int index_prev = FT_Get_Char_Index(typeface, cprev);
		if (!index_prev)
			continue;
		for (int j = 0; j < f->total_glyphs; j++) {
			uint32_t cnext = charset[j];
			struct gltext_glyph *next = f->glyph_array + cprev;
			int index_next = FT_Get_Char_Index(typeface, cnext);
			if (!index_next)
				continue;
			int16_t *k = f->kerning_table + (i * f->total_glyphs) + j;
			FT_Vector delta;

			if (!FT_Get_Kerning(typeface,
					index_prev,
					index_next,
					FT_KERNING_DEFAULT,
					&delta)) {
				*k = delta.x;
			}
		}
	}

	int pot_size;

	max_dim += 16;

	if (max_dim < 16) {
		pot_size = 16;
	} else {
		pot_size = 1 << (32 - __builtin_clz(max_dim));
	}
	f->pot_size = pot_size;
	f->glyph_array['\n'].font = f;

	int texels_per_layer = pot_size * pot_size;
	uint8_t *atlas_buffer = (uint8_t *)calloc(f->total_glyphs, texels_per_layer);
	int8_t *glyph_metric_array = (int8_t *)malloc(f->total_glyphs * sizeof(int8_t) * 4);
	int8_t *glyph_metric_ptr = glyph_metric_array;

	double *srcf = (double *)malloc(texels_per_layer * sizeof(double));
	short *distx = (short *)malloc(texels_per_layer * sizeof(short));
	short *disty = (short *)malloc(texels_per_layer * sizeof(short)) ;
	double *gx = (double *)malloc(texels_per_layer * sizeof(double));
	double *gy = (double *)malloc(texels_per_layer * sizeof(double));
	double *dist = (double *)malloc(texels_per_layer * sizeof(double));

	uint8_t *layer_ptr = atlas_buffer;
	for (int i = 0; i < f->total_glyphs; i++) {
		uint32_t c = charset[i];
		struct gltext_glyph *g = f->glyph_array + c;
		int gindex = FT_Get_Char_Index(typeface, c);
		if (gindex) {
			uint8_t *dest = layer_ptr + (pot_size * 8) + 8;
			uint8_t *source = g->bitmap_bits;
			for (int i = 0; i < g->bitmap_height; i++) {
				memcpy(dest, source, g->bitmap_width);
				dest += pot_size;
				source += g->bitmap_pitch;
			}

			uint8_t *temp = layer_ptr;
			int index = 0;
			for (int i = 0; i < g->bitmap_height + 16; i++) {
				for (int j = 0; j < g->bitmap_width + 16; j++) {
					srcf[index++] = (temp[j]*1.0)/256;
				}
				temp += pot_size;
			}
			computegradient(srcf, g->bitmap_width + 16, g->bitmap_height + 16, gx, gy);
			edtaa3(srcf, gx, gy, g->bitmap_width + 16, g->bitmap_height + 16, distx, disty, dist);

			temp = layer_ptr;
			index = 0;
			for (int i = 0; i < g->bitmap_height + 16; i++) {
				for (int j = 0; j < g->bitmap_width + 16; j++) {
					float s = (float)dist[index++];
					int val = (int)(128 - s*16.0);
					if (val < 0) val = 0;
					if (val > 255) val = 255;
					temp[j] = val;
				}
				temp += pot_size;
			}
		}
		*(glyph_metric_ptr++) = (int8_t)g->bitmap_width;
		*(glyph_metric_ptr++) = (int8_t)g->bitmap_height;
		*(glyph_metric_ptr++) = (int8_t)g->left;
		*(glyph_metric_ptr++) = (int8_t)g->top;
		layer_ptr += texels_per_layer;
	}

	free(srcf);
	free(distx);
	free(disty);
	free(gx);
	free(gy);
	free(dist);
	f->glyph_metric_array = glyph_metric_array;
	f->atlas_buffer = atlas_buffer;

	return f;
}

bool gltext_font_free(gltext_font_t font)
{
	gltext_font_destroy_texture(font);
	free(font->atlas_buffer);
	free(font->glyph_metric_array);
	free(font->kerning_table);
	free(font);
	return true;
}
