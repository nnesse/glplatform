#include "gltext.h"
#include "priv.h"

#include <math.h>

#include "edtaa3func.c"

#define GLPLATFORM_GL_VERSION 33
#include "glcore.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <string.h>
#include <stdio.h>

#define FONT_FORMAT_VERSION 1

enum vertex_attrib_locations {
	POS_LOC = 0,
	GLYPH_INDEX_LOC = 1
};

//
// font
//
struct gltext_font {
	int size;
	struct gltext_glyph **glyph_map;
	int total_glyphs;
	struct gltext_glyph *glyphs;
	int8_t *glyph_metric_array;
	uint8_t *atlas_buffer;
	int pot_size;
	GLuint atlas_texture;
	GLuint glyph_metric_texture;
	GLuint glyph_metric_texture_buffer;
	int16_t *kerning_table;
	int max_char;
	bool sdf;
};

const struct gltext_glyph *gltext_get_glyph(gltext_font_t font, char32_t c)
{
	struct gltext_glyph *g;
	if (c > font->max_char) {
		return NULL;
	}
        return font->glyph_map[c];
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
	int sdf_loc;
	gltext_font_t cur_font;
};

static bool init_renderer(struct gltext_renderer *inst);

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
	inst->cur_font = font;
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
	glUniform1i(inst->sdf_loc, inst->cur_font->sdf);
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
		return false;
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
		"uniform bool sdf;\n"
		"uniform isamplerBuffer glyph_metric_sampler;\n"
		"out vec3 texcoord_f;\n"
		"\n"
		"void genVertex(vec2 ul, vec2 corner, vec2 size, vec3 texcoord)\n"
		"{\n"
			"gl_Position = mvp * vec4(ul + (corner * size), 0, 1);\n"
			"texcoord_f = texcoord + vec3(corner * size, 0);\n"
			"EmitVertex();\n"
		"}\n"
		"\n"
		"void main()\n"
		"{\n"
			"if (glyph_index[0] >= 0) {\n"
				"vec4 glyph_metrics = texelFetch(glyph_metric_sampler, glyph_index[0]);\n"
				"vec2 size = glyph_metrics.xy;\n"
				"vec2 ul = pos[0] + vec2(glyph_metrics.z, -glyph_metrics.w);\n"
				"if (sdf) {\n"
				"	size += vec2(16, 16);\n"
				"	ul -= vec2(8, 8);\n"
				"}\n"
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
		"uniform bool sdf;\n"
		"void main()\n"
		"{\n"
			"ivec3 tex_size = textureSize(sampler, 0);\n"
			"float texel;\n"
			"if (sdf) {\n"
			"	float D = texture(sampler, vec3(texcoord_f.xy/tex_size.xy, texcoord_f.z)).r - 0.50;\n"
			"	float aastep = length(vec2(dFdx(D), dFdy(D)));\n"
			"	texel = smoothstep(-aastep, aastep, D);\n"
			"} else {\n"
			"	texel = texture(sampler, vec3(texcoord_f.xy/tex_size.xy, texcoord_f.z)).r;\n"
			"}\n"
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
	inst->sdf_loc = glGetUniformLocation(inst->glsl_program, "sdf");
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
		font->atlas_texture = 0;
	}
}

#define HEADER_ENT 6
#define GLYPH_ENT 7

gltext_font_t gltext_font_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint32_t header[HEADER_ENT];
	int rd_len = HEADER_ENT;
	int count = fread(header, sizeof(uint32_t), rd_len, f);
	if (count != rd_len)
		goto error1;
	gltext_font_t font = calloc(sizeof(struct gltext_font), 1);
	if (!font)
		goto error1;
	if (header[0] != FONT_FORMAT_VERSION)
		goto error2;
	font->size = header[5];
	font->total_glyphs = header[2];
	font->pot_size = header[3];
	font->max_char = header[4];
	font->sdf = header[1];

	font->glyphs = calloc(sizeof(struct gltext_glyph), font->total_glyphs);
	if (!font->glyphs)
		goto error2;
	font->glyph_map = calloc(sizeof(struct gltext_glyph *), font->max_char + 1);
	if (!font->glyph_map)
		goto error2;
	font->glyph_metric_array = (int8_t *)calloc(sizeof(int8_t) * 4, font->total_glyphs);
	if (!font->glyph_metric_array)
		goto error2;
	int8_t *glyph_metric_ptr = font->glyph_metric_array;
	for (int i = 0; i < font->total_glyphs; i++) {
		uint32_t glyph_data[GLYPH_ENT];
		rd_len = GLYPH_ENT;
		count = fread(glyph_data, sizeof(uint32_t), rd_len, f);
		if (count != rd_len)
			goto error2;
		struct gltext_glyph *g = font->glyphs + i;
		g->character = glyph_data[0];
		g->left = glyph_data[1];
		g->top = glyph_data[2];
		g->advance_x = glyph_data[3];
		g->advance_y = glyph_data[4];
		g->bitmap_width = glyph_data[5];
		g->bitmap_height = glyph_data[6];
		g->w = i;
		g->font = font;
		*(glyph_metric_ptr++) = (int8_t)g->bitmap_width;
		*(glyph_metric_ptr++) = (int8_t)g->bitmap_height;
		*(glyph_metric_ptr++) = (int8_t)g->left;
		*(glyph_metric_ptr++) = (int8_t)g->top;
		font->glyph_map[g->character] = g;
	}

	rd_len = font->total_glyphs * font->total_glyphs;
	font->kerning_table = calloc(sizeof(int16_t), rd_len);
	if (!font->kerning_table)
		goto error2;
	count = fread(font->kerning_table, sizeof(int16_t), rd_len, f);
	if (count != rd_len)
		goto error2;
	rd_len = font->pot_size * font->pot_size * font->total_glyphs;
	font->atlas_buffer = calloc(sizeof(uint8_t), rd_len);
	if (!font->atlas_buffer)
		goto error2;
	count = fread(font->atlas_buffer, sizeof(uint8_t), rd_len, f);
	if (count != rd_len)
		goto error2;
	fclose(f);
	return font;
error2:
	free(font->atlas_buffer);
	free(font->kerning_table);
	free(font->glyph_metric_array);
	free(font->glyph_map);
	free(font->glyphs);
	free(font);
error1:
	fclose(f);
	return NULL;
}

bool gltext_font_store(gltext_font_t font, const char *path)
{
	if (!font || !font->kerning_table || !font->atlas_buffer)
		return false;

	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	uint32_t header[HEADER_ENT];
	header[0] = FONT_FORMAT_VERSION;
	header[1] = font->sdf;
	header[2] = font->total_glyphs;
	header[3] = font->pot_size;
	header[4] = font->max_char;
	header[5] = font->size;
	int wr_len = HEADER_ENT;
	int count = fwrite(header, sizeof(uint32_t), wr_len, f);
	if (count != wr_len) {
		fclose(f);
		return false;
	}

	for (int i = 0; i < font->total_glyphs; i++) {
		struct gltext_glyph *g = font->glyphs + i;
		uint32_t glyph[GLYPH_ENT];
		glyph[0] = g->character;
		glyph[1] = g->left;
		glyph[2] = g->top;
		glyph[3] = g->advance_x;
		glyph[4] = g->advance_y;
		glyph[5] = g->bitmap_width;
		glyph[6] = g->bitmap_height;
		int wr_len = GLYPH_ENT;
		int count = fwrite(glyph, sizeof(uint32_t), wr_len, f);
		if (count != wr_len) {
			fclose(f);
			return false;
		}
	}

	wr_len = font->total_glyphs * font->total_glyphs;
	count = fwrite(font->kerning_table, sizeof(int16_t), wr_len, f);
	if (count != wr_len) {
		fclose(f);
		return false;
	}

	wr_len = font->pot_size * font->pot_size * font->total_glyphs;
	count = fwrite(font->atlas_buffer, sizeof(uint8_t), wr_len, f);
	if (count != wr_len) {
		fclose(f);
		return false;
	}

	fclose(f);
	return true;
}

gltext_font_t gltext_font_create(const char32_t *charset,
	gltext_typeface_t typeface_,
	int size, bool sdf)
{
	double *srcf = NULL;
	short *distx = NULL;
	short *disty = NULL;
	double *gx = NULL;
	double *gy = NULL;
	double *dist = NULL;
	int8_t *glyph_metric_array = NULL;
	uint8_t *atlas_buffer = NULL;

	FT_Face typeface = (FT_Face) typeface_;
	struct gltext_renderer *inst = get_renderer();
	uint8_t **bitmap_bits = NULL;
	int *bitmap_pitch = NULL;
	if (!inst)
		return NULL;

	if (!typeface)
		return NULL;

	if (FT_Select_Charmap(typeface, ft_encoding_unicode))
		return NULL;

	if (FT_Set_Pixel_Sizes(typeface, size, size))
		return NULL;

	struct gltext_font *f = (struct gltext_font *)calloc(sizeof(struct gltext_font), 1);
	if (!f)
		return NULL;

	f->size = size;
	f->sdf = sdf;
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
	f->glyphs = calloc(f->total_glyphs, sizeof(struct gltext_glyph));
	f->glyph_map = calloc((f->max_char + 1), sizeof(struct gltext_glyph *));
	f->kerning_table = (int16_t *)calloc(f->total_glyphs * f->total_glyphs, sizeof(int16_t));

	bitmap_bits = (uint8_t **)calloc(sizeof(uint8_t *), f->total_glyphs);
	bitmap_pitch = (int *)calloc(sizeof(int), f->total_glyphs);

	if (!f->glyphs || !f->glyph_map || !f->kerning_table || !bitmap_bits || !bitmap_pitch)
		goto error;

	int max_dim = 0;

	for (int i = 0; i < f->total_glyphs; i++) {
		uint32_t c = charset[i];
		struct gltext_glyph *g = f->glyphs + i;
		g->w = i;
		g->character = c;
		int index = FT_Get_Char_Index(typeface, c);
		if (!index)
			continue;
		f->glyph_map[c] = g;
		FT_Load_Glyph(typeface, index, 0/* flags */);
		FT_Render_Glyph(typeface->glyph, FT_RENDER_MODE_NORMAL);
		g->bitmap_width = typeface->glyph->bitmap.width;
		g->bitmap_height = typeface->glyph->bitmap.rows;
		bitmap_pitch[i] = typeface->glyph->bitmap.pitch;
		size_t image_size = bitmap_pitch[i] * g->bitmap_height;
		bitmap_bits[i] = (uint8_t *)malloc(image_size);
		if (!bitmap_bits[i])
			goto error;
		memcpy(bitmap_bits[i], typeface->glyph->bitmap.buffer, image_size);
		g->left = typeface->glyph->bitmap_left;
		g->top = typeface->glyph->bitmap_top;
		g->advance_x = typeface->glyph->advance.x;
		g->advance_y = typeface->glyph->advance.y;
		g->font = f;
		max_dim = g->bitmap_width > max_dim ? g->bitmap_width : max_dim;
		max_dim = g->bitmap_height > max_dim ? g->bitmap_height : max_dim;
	}

	for (int i = 0; i < f->total_glyphs; i++) {
		uint32_t cprev = charset[i];
		struct gltext_glyph *prev = f->glyphs + i;
		int index_prev = FT_Get_Char_Index(typeface, cprev);
		if (!index_prev)
			continue;
		for (int j = 0; j < f->total_glyphs; j++) {
			uint32_t cnext = charset[j];
			struct gltext_glyph *next = f->glyphs + j;
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

	if (f->sdf) {
		max_dim += 16;
	}

	if (max_dim < 16) {
		pot_size = 16;
	} else {
		pot_size = 1 << (32 - __builtin_clz(max_dim));
	}
	f->pot_size = pot_size;

	int texels_per_layer = pot_size * pot_size;
	atlas_buffer = (uint8_t *)calloc(sizeof(uint8_t *), f->total_glyphs * texels_per_layer);
	glyph_metric_array = (int8_t *)calloc(f->total_glyphs, sizeof(int8_t) * 4);

	if (!atlas_buffer || !glyph_metric_array)
		goto error;

	int8_t *glyph_metric_ptr = glyph_metric_array;

	if (f->sdf) {
		srcf = (double *)malloc(texels_per_layer * sizeof(double));
		distx = (short *)malloc(texels_per_layer * sizeof(short));
		disty = (short *)malloc(texels_per_layer * sizeof(short));
		gx = (double *)malloc(texels_per_layer * sizeof(double));
		gy = (double *)malloc(texels_per_layer * sizeof(double));
		dist = (double *)malloc(texels_per_layer * sizeof(double));
		if (!srcf || !distx || !disty || !gx || !gy || !dist)
			goto error;
	}

	uint8_t *layer_ptr = atlas_buffer;
	for (int i = 0; i < f->total_glyphs; i++) {
		uint32_t c = charset[i];
		struct gltext_glyph *g = f->glyphs + i;
		int gindex = FT_Get_Char_Index(typeface, c);
		if (gindex) {
			uint8_t *dest;
			if (f->sdf)
				dest = layer_ptr + (pot_size * 8) + 8;
			else
				dest = layer_ptr;
			uint8_t *source = bitmap_bits[i];
			for (int j = 0; j < g->bitmap_height; j++) {
				memcpy(dest, source, g->bitmap_width);
				dest += pot_size;
				source += bitmap_pitch[i];
			}

			if (f->sdf) {
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
	for (int i = 0; i < f->total_glyphs; i++) {
		free(bitmap_bits[i]);
	}
	free(bitmap_bits);
	free(bitmap_pitch);
	f->glyph_metric_array = glyph_metric_array;
	f->atlas_buffer = atlas_buffer;

	return f;
error:
	if (bitmap_bits) {
		for (int i = 0; i < f->total_glyphs; i++) {
			free(bitmap_bits[i]);
		}
	}
	free(bitmap_bits);
	free(bitmap_pitch);
	free(atlas_buffer);
	free(glyph_metric_array);
	free(srcf);
	free(distx);
	free(disty);
	free(gx);
	free(gy);
	free(dist);
	if (f) {
		free(f->glyph_map);
		free(f->glyphs);
		free(f->kerning_table);
	}
	free(f);
	return NULL;
}

bool gltext_font_free(gltext_font_t font)
{
	gltext_font_destroy_texture(font);
	free(font->atlas_buffer);
	free(font->glyph_metric_array);
	free(font->kerning_table);
	free(font->glyph_map);
	free(font->glyphs);
	free(font);
	return true;
}
