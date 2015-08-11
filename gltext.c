#include "gltext.h"

#include <math.h>

#include "edtaa3func.c"

#define GLB_GL_VERSION 33
#include "glb-glcore.h"

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
	GLuint vertex_shader;
	GLuint geometry_shader;
	GLuint atlas_texture_name;
	GLuint gl_vertex_array;
	GLuint stream_vbo;
	GLuint texcoord_texture;
	GLuint texcoord_texture_buffer;
	GLuint glyph_size_texture;
	GLuint glyph_size_texture_buffer;
	int sampler_loc;
	int color_loc;
	int glyph_size_sampler_loc;
	int mvp_loc;
	int num_chars;

	const char *charset;

	uint8_t *atlas_buffer;
	bool *layer_loaded;
	bool initialized;
};

#define TEXTURE_SIZE 128
#define TEXELS_PER_LAYER (TEXTURE_SIZE * TEXTURE_SIZE)

float gltext_get_advance(const struct gltext_glyph *prev, const struct gltext_glyph *next)
{
	float ret = 0;
	if (prev) {
		int d = prev->advance_x;
		if (next && prev->font == next->font) {
			FT_Vector delta;
			const struct gltext_font *font = prev->font;
			if (!FT_Get_Kerning(font->typeface,
					prev->typeface_index,
					next->typeface_index,
					FT_KERNING_DEFAULT,
					&delta)) {
				d += delta.x;
			}
		}
		ret += d/64.0;
	}
	return ret;
}

struct gltext_glyph_instance *gltext_renderer_prepare_render(gltext_renderer_t renderer_, const struct gltext_font *font, int num_chars)
{
	struct renderer *inst = (struct renderer *)renderer_;
	if (!inst->initialized)
		return NULL;

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

void gltext_renderer_submit_render(gltext_renderer_t renderer, const struct gltext_color *color, const float *mvp)
{
	struct renderer *inst = (struct renderer *)renderer;
	glUnmapBuffer(GL_ARRAY_BUFFER);
	glUseProgram(inst->glsl_program);
	glBindVertexArray(inst->gl_vertex_array);
	glUniformMatrix4fv(inst->mvp_loc, 1, GL_FALSE, mvp);
	glUniform4fv(inst->color_loc, 1, (GLfloat *)color);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, inst->atlas_texture_name);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_BUFFER, inst->texcoord_texture);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_BUFFER, inst->glyph_size_texture);
	glDrawArrays(GL_POINTS, 0, inst->num_chars);
}

gltext_renderer_t gltext_renderer_new()
{
	struct renderer *inst = malloc(sizeof(struct renderer));
	memset(inst, 0, sizeof(*inst));
	inst->ft_library = NULL;
	inst->glsl_program = 0;
	inst->fragment_shader = 0;
	inst->vertex_shader = 0;
	inst->geometry_shader = 0;
	inst->atlas_texture_name = 0;
	inst->initialized = false;
	FT_Init_FreeType(&inst->ft_library);
	return inst;
}

void gltext_renderer_free(gltext_renderer_t renderer)
{
	struct renderer *inst = (struct renderer *)renderer;
	if (inst->fragment_shader)
		glDeleteShader(inst->fragment_shader);
	if (inst->geometry_shader)
		glDeleteShader(inst->geometry_shader);
	if (inst->vertex_shader)
		glDeleteShader(inst->vertex_shader);
	if (inst->glsl_program)
		glDeleteProgram(inst->glsl_program);
	if (inst->ft_library)
		FT_Done_FreeType(inst->ft_library);
	if (inst->atlas_texture_name)
		glDeleteTextures(1, &inst->atlas_texture_name);
	free(inst);
}

gltext_typeface_t gltext_renderer_get_typeface(gltext_renderer_t renderer, const char *path)
{
	FT_Face face;
	struct renderer *inst = (struct renderer *)renderer;
	int error = FT_New_Face(inst->ft_library, path, 0, &face);
	if (error) {
		return NULL;
	}
	return face;
}

static bool init_program(struct renderer *inst)
{
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
		"uniform isamplerBuffer glyph_size_sampler;\n"
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
				"vec4 glyph_metrics = texelFetch(glyph_size_sampler, glyph_index[0]);\n"
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
			"float D, dfdx, dfdy;\n"
			"ivec3 tex_size = textureSize(sampler, 0);\n"
			"float D00 = texture(sampler, vec3(vec2(texcoord_f.xy) / tex_size.xy, texcoord_f.z)).r;\n"
			"float D01 = texture(sampler, vec3((vec2(texcoord_f.xy) + dFdx(texcoord_f.xy)) / tex_size.xy, texcoord_f.z)).r;\n"
			"float D10 = texture(sampler, vec3((vec2(texcoord_f.xy) + dFdy(texcoord_f.xy)) / tex_size.xy, texcoord_f.z)).r;\n"
			"D = D00 = (D00 - 0.50) * 16.0;\n"
			"D01 = (D01 - 0.50) * 16.0;\n"
			"D10 = (D10 - 0.50) * 16.0;\n"
			"dfdx = -(D01 - D00);\n"
			"dfdy = -(D10 - D00);\n"
			"float aastep = length(vec2(dfdx, dfdy));\n"
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

	inst->vertex_shader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(inst->vertex_shader, 1, (const char **)&vertex_shader_text, NULL);
	glCompileShader(inst->vertex_shader);
	glGetShaderiv(inst->vertex_shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		char info_log[1000];
		glGetShaderInfoLog(inst->vertex_shader, sizeof(info_log), NULL, info_log);
		printf("renderer: Vertex shader compile failed\n%s", info_log);
		glDeleteShader(inst->fragment_shader);
		inst->fragment_shader = 0;
		glDeleteShader(inst->vertex_shader);
		inst->vertex_shader = 0;
		glDeleteShader(inst->geometry_shader);
		inst->geometry_shader = 0;
		return false;
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
		glDeleteShader(inst->fragment_shader);
		inst->fragment_shader = 0;
		glDeleteShader(inst->vertex_shader);
		inst->vertex_shader = 0;
		glDeleteProgram(inst->glsl_program);
		inst->glsl_program = 0;
		return false;
	}

	glEnableVertexAttribArray(GLYPH_INDEX_LOC);
	glEnableVertexAttribArray(POS_LOC);
	glEnableVertexAttribArray(COLOR_LOC);

	glGenBuffers(1, &inst->stream_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, inst->stream_vbo);

	glVertexAttribPointer(POS_LOC,
		2,
		GL_FLOAT,
		GL_FALSE,
		sizeof(struct gltext_glyph_instance),
		(void *)offsetof(struct gltext_glyph_instance, pos));

	glVertexAttribPointer(COLOR_LOC,
		4,
		GL_FLOAT,
		GL_FALSE,
		sizeof(struct gltext_glyph_instance),
		(void *)offsetof(struct gltext_glyph_instance, color));

	glVertexAttribIPointer(GLYPH_INDEX_LOC,
		1,
		GL_UNSIGNED_INT,
		sizeof(struct gltext_glyph_instance),
		(void *)offsetof(struct gltext_glyph_instance, w));

	//Cache uniform locations
	inst->mvp_loc = glGetUniformLocation(inst->glsl_program, "mvp");
	inst->sampler_loc = glGetUniformLocation(inst->glsl_program, "sampler");
	inst->color_loc = glGetUniformLocation(inst->glsl_program, "color");
	inst->glyph_size_sampler_loc = glGetUniformLocation(inst->glsl_program, "glyph_size_sampler");

	glUseProgram(inst->glsl_program);
	glUniform1i(inst->sampler_loc, 0);
	glUniform1i(inst->glyph_size_sampler_loc, 2);
	return true;
}

bool gltext_renderer_initialize(gltext_renderer_t renderer,
	const char *charset,
	const struct gltext_font_desc *font_descriptions,
	int count,
	struct gltext_font *fonts)
{
	struct renderer *inst = (struct renderer *)renderer;
	if (inst->initialized)
		return false; //You can't reinitialize the renderer

	if (!inst->ft_library)
		return false;

	inst->charset = charset;

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
		struct gltext_font *f = fonts + i;
		const struct gltext_font_desc *font_desc = font_descriptions + i;
		FT_Face typeface = (FT_Face) font_desc->typeface;
		int size = font_desc->size;
		int max_char = 0;

		if (!typeface)
			return false;

		for (const char *c = inst->charset; *c; c++)
			if (*c > max_char)
				max_char = *c;

		if ('\n' > max_char)
			max_char = '\n';

		f->glyph_array = malloc((max_char + 1) * sizeof(struct gltext_glyph));
		f->size = size;
		f->typeface = typeface;
		f->renderer = renderer;
		FT_Set_Pixel_Sizes(typeface, size, size);

		for (const char *c = inst->charset; *c; c++) {
			struct gltext_glyph *g = f->glyph_array + (*c);
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
	}

	//
	// Assign glyphs to layers in the texture array
	//
	int w = 0;
	for (i = 0; i < count; i++) {
		const struct gltext_font *f = fonts + i;
		const struct gltext_font_desc *font_desc = font_descriptions + i;
		for (const char *c = inst->charset; *c; c++) {
			struct gltext_glyph *g = f->glyph_array + (*c);
			g->w = w++;
			g->font = f;
		}
	}
	int tex_array_size = w + 1;

	//
	// Blit the glyph bitmaps into a CPU texture
	//
	inst->atlas_buffer = (uint8_t *)calloc(tex_array_size, TEXTURE_SIZE * TEXTURE_SIZE);
	int8_t *glyph_size_array = (int8_t *)malloc(total_glyphs * sizeof(int16_t) * 4);
	int8_t *glyph_size_ptr = glyph_size_array;
	static double srcf[TEXELS_PER_LAYER];
	static short distx[TEXELS_PER_LAYER];
	static short disty[TEXELS_PER_LAYER];
	static double gx[TEXELS_PER_LAYER];
	static double gy[TEXELS_PER_LAYER];
	static double dist[TEXELS_PER_LAYER];

	for (i = 0; i < count; i++) {
		const struct gltext_font *f = fonts + i;
		const struct gltext_font_desc *font_desc = font_descriptions + i;
		for (const char *c = inst->charset; *c; c++) {
			struct gltext_glyph *g = f->glyph_array + (*c);
			uint8_t *dest = inst->atlas_buffer + (TEXTURE_SIZE * 8) + 8 + (g->w * TEXELS_PER_LAYER);
			uint8_t *source = g->bitmap_bits;
			for (int i = 0; i < g->bitmap_height; i++) {
				memcpy(dest, source, g->bitmap_width);
				dest += TEXTURE_SIZE;
				source += g->bitmap_pitch;
			}

			uint8_t *temp = inst->atlas_buffer + (g->w * TEXELS_PER_LAYER);
			int index = 0;
			for (int i = 0; i < g->bitmap_height + 16; i++) {
				for (int j = 0; j < g->bitmap_width + 16; j++) {
					srcf[index++] = (temp[j]*1.0)/256;
				}
				temp += TEXTURE_SIZE;
			}
			computegradient(srcf, g->bitmap_width + 16, g->bitmap_height + 16, gx, gy);
			edtaa3(srcf, gx, gy, g->bitmap_width + 16, g->bitmap_height + 16, distx, disty, dist);
	
			temp = inst->atlas_buffer + (g->w * TEXELS_PER_LAYER);
			index = 0;
			for (int i = 0; i < g->bitmap_height + 16; i++) {
				for (int j = 0; j < g->bitmap_width + 16; j++) {
					float s = dist[index++];
					int val = 128 - s*16.0;
					if (val < 0) val = 0;
					if (val > 255) val = 255;
					temp[j] = val;
				}
				temp += TEXTURE_SIZE;
			}
			*(glyph_size_ptr++) = (int8_t)g->bitmap_width;
			*(glyph_size_ptr++) = (int8_t)g->bitmap_height;
			*(glyph_size_ptr++) = (int8_t)g->left;
			*(glyph_size_ptr++) = (int8_t)g->top;
		}
	}

	//
	//Setup glyph size texture buffer
	//
	glGenBuffers(1, &inst->glyph_size_texture_buffer);
	glBindBuffer(GL_TEXTURE_BUFFER, inst->glyph_size_texture_buffer);
	glBufferData(GL_TEXTURE_BUFFER, total_glyphs * 4, glyph_size_array, GL_STATIC_DRAW);
	glGenTextures(1, &inst->glyph_size_texture);
	glBindTexture(GL_TEXTURE_BUFFER, inst->glyph_size_texture);
	glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8I, inst->glyph_size_texture_buffer);
	free(glyph_size_array);

	//
	// Allocate and altas texture and setup texture filtering
	//
	glGenTextures(1, &inst->atlas_texture_name);
	glBindTexture(GL_TEXTURE_2D_ARRAY, inst->atlas_texture_name);
	glTexImage3D(GL_TEXTURE_2D_ARRAY,
		0, /* layer */
		GL_R8,
		TEXTURE_SIZE, TEXTURE_SIZE, tex_array_size /* uvw size */,
		0, /* border */
		GL_RED,
		GL_UNSIGNED_BYTE,
		inst->atlas_buffer);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

	inst->initialized = true;
	return inst->initialized;
}
