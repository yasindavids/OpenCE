/*
ANGLE_CAPS.C

What the OpenGL ES renderer (port/linux/src/d3d8_gl.c, the HALO_ANDROID
path) gets from ANGLE's Metal backend on this Mac, and what a draw costs
the CPU. The program loads libEGL.dylib and libGLESv2.dylib from a folder,
for example the copy of ANGLE inside an Electron app, asks for the Metal
backend, and makes an ES 3 context on a 1280x720 pbuffer. It prints the
version, the extensions that gl_initialize checks, and the time of a loop
of draws, each with a uniform change, as the renderer makes them.

	clang -O2 angle_caps.c -o angle_caps
	./angle_caps "/Applications/Visual Studio Code.app/Contents/Frameworks/Electron Framework.framework/Libraries"

Result on an M1 MacBook Air, macOS 26.6.1: refer to port/macos/PROPOSAL.md.
*/

#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30fb
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489

#define GL_RENDERER 0x1f01
#define GL_VERSION 0x1f02
#define GL_EXTENSIONS 0x1f03
#define GL_NUM_EXTENSIONS 0x821d
#define GL_MAX_FRAGMENT_ATOMIC_COUNTERS 0x92d6
#define GL_VERTEX_SHADER 0x8b31
#define GL_FRAGMENT_SHADER 0x8b30
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88e4
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_LINK_STATUS 0x8b82

typedef void *(*egl_get_platform_display_t)(unsigned, void *, const intptr_t *);
typedef unsigned (*egl_initialize_t)(void *, int *, int *);
typedef unsigned (*egl_choose_config_t)(void *, const int *, void **, int, int *);
typedef void *(*egl_create_pbuffer_surface_t)(void *, void *, const int *);
typedef void *(*egl_create_context_t)(void *, void *, void *, const int *);
typedef unsigned (*egl_make_current_t)(void *, void *, void *, void *);
typedef const char *(*egl_query_string_t)(void *, int);

static void *gles;

static void *gl(const char *name)
{
	void *function = dlsym(gles, name);

	if (!function)
	{
		printf("missing %s\n", name);
		exit(1);
	}
	return function;
}

int main(int argc, char **argv)
{
	static const char *checked[] = {
		"GL_EXT_texture_compression_s3tc",
		"GL_EXT_texture_compression_dxt1",
		"GL_ANGLE_texture_compression_dxt3",
		"GL_ANGLE_texture_compression_dxt5",
		"GL_EXT_copy_image",
		"GL_OES_copy_image",
		"GL_EXT_texture_border_clamp",
		"GL_OES_texture_border_clamp",
		"GL_EXT_texture_filter_anisotropic",
		"GL_EXT_draw_elements_base_vertex",
		"GL_OES_draw_elements_base_vertex",
		"GL_EXT_buffer_storage",
		"GL_EXT_clip_control",
		"GL_EXT_occlusion_query_boolean",
		"GL_EXT_disjoint_timer_query",
	};
	static const intptr_t display_attributes[] = {
		EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
	static const int config_attributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
	static const int surface_attributes[] = { EGL_WIDTH, 1280, EGL_HEIGHT, 720, EGL_NONE };
	static const int context_attributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
	static const char *vertex_source =
		"#version 300 es\nin vec2 p; uniform vec4 u; void main() { gl_Position = vec4(p * u.xy + u.zw, 0.0, 1.0); }\n";
	static const char *fragment_source =
		"#version 300 es\nprecision mediump float; uniform highp vec4 u; out vec4 c; void main() { c = u; }\n";
	static const float triangle[] = { -0.01f, -0.01f, 0.01f, -0.01f, 0.0f, 0.01f };
	char path[1024];
	void *egl, *display, *config, *surface, *context;
	int major, minor, configs, count, index, draws = 20000;
	unsigned shaders[2], program, buffer, link_status = 0;
	int uniform;
	mach_timebase_info_data_t timebase;
	uint64_t start, end;

	if (argc < 2)
	{
		printf("usage: angle_caps <folder with libEGL.dylib and libGLESv2.dylib>\n");
		return 1;
	}
	snprintf(path, sizeof(path), "%s/libGLESv2.dylib", argv[1]);
	gles = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
	snprintf(path, sizeof(path), "%s/libEGL.dylib", argv[1]);
	egl = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
	if (!gles || !egl)
	{
		printf("dlopen: %s\n", dlerror());
		return 1;
	}

	display = ((egl_get_platform_display_t)dlsym(egl, "eglGetPlatformDisplay"))(
		EGL_PLATFORM_ANGLE_ANGLE, NULL, display_attributes);
	if (!display || !((egl_initialize_t)dlsym(egl, "eglInitialize"))(display, &major, &minor))
	{
		printf("no Metal display\n");
		return 1;
	}
	((egl_choose_config_t)dlsym(egl, "eglChooseConfig"))(display, config_attributes, &config, 1, &configs);
	surface = ((egl_create_pbuffer_surface_t)dlsym(egl, "eglCreatePbufferSurface"))(display, config, surface_attributes);
	context = ((egl_create_context_t)dlsym(egl, "eglCreateContext"))(display, config, NULL, context_attributes);
	if (!configs || !surface || !context ||
		!((egl_make_current_t)dlsym(egl, "eglMakeCurrent"))(display, surface, surface, context))
	{
		printf("no ES 3 context\n");
		return 1;
	}

	{
		const char *(*get_string)(unsigned) = gl("glGetString");
		const char *(*get_stringi)(unsigned, unsigned) = gl("glGetStringi");
		void (*get_integerv)(unsigned, int *) = gl("glGetIntegerv");
		int extensions = 0, counters = 0;
		unsigned check;

		printf("EGL %d.%d, %s\n", major, minor, ((egl_query_string_t)dlsym(egl, "eglQueryString"))(display, 0x3054));
		printf("GL_VERSION %s\nGL_RENDERER %s\n", get_string(GL_VERSION), get_string(GL_RENDERER));
		get_integerv(GL_NUM_EXTENSIONS, &extensions);
		get_integerv(GL_MAX_FRAGMENT_ATOMIC_COUNTERS, &counters);
		printf("%d extensions, GL_MAX_FRAGMENT_ATOMIC_COUNTERS %d\n", extensions, counters);
		for (check = 0; check < sizeof(checked) / sizeof(checked[0]); check++)
		{
			int found = 0;

			for (index = 0; index < extensions; index++)
				if (!strcmp(get_stringi(GL_EXTENSIONS, (unsigned)index), checked[check]))
					found = 1;
			printf("  %-36s %s\n", checked[check], found ? "yes" : "no");
		}
	}

	{
		unsigned (*create_shader)(unsigned) = gl("glCreateShader");
		void (*shader_source)(unsigned, int, const char **, const int *) = gl("glShaderSource");
		void (*compile_shader)(unsigned) = gl("glCompileShader");
		unsigned (*create_program)(void) = gl("glCreateProgram");
		void (*attach_shader)(unsigned, unsigned) = gl("glAttachShader");
		void (*bind_attrib_location)(unsigned, unsigned, const char *) = gl("glBindAttribLocation");
		void (*link_program)(unsigned) = gl("glLinkProgram");
		void (*get_programiv)(unsigned, unsigned, unsigned *) = gl("glGetProgramiv");
		void (*use_program)(unsigned) = gl("glUseProgram");
		int (*get_uniform_location)(unsigned, const char *) = gl("glGetUniformLocation");
		void (*gen_buffers)(int, unsigned *) = gl("glGenBuffers");
		void (*bind_buffer)(unsigned, unsigned) = gl("glBindBuffer");
		void (*buffer_data)(unsigned, long, const void *, unsigned) = gl("glBufferData");
		void (*enable_vertex_attrib_array)(unsigned) = gl("glEnableVertexAttribArray");
		void (*vertex_attrib_pointer)(unsigned, int, unsigned, unsigned char, int, const void *) = gl("glVertexAttribPointer");
		void (*uniform4f)(int, float, float, float, float) = gl("glUniform4f");
		void (*draw_arrays)(unsigned, int, int) = gl("glDrawArrays");
		void (*clear)(unsigned) = gl("glClear");
		void (*finish)(void) = gl("glFinish");
		const char *sources[2] = { vertex_source, fragment_source };
		int frame;

		program = create_program();
		for (index = 0; index < 2; index++)
		{
			shaders[index] = create_shader(index ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
			shader_source(shaders[index], 1, &sources[index], NULL);
			compile_shader(shaders[index]);
			attach_shader(program, shaders[index]);
		}
		bind_attrib_location(program, 0, "p");
		link_program(program);
		get_programiv(program, GL_LINK_STATUS, &link_status);
		if (!link_status)
		{
			printf("link failed\n");
			return 1;
		}
		use_program(program);
		uniform = get_uniform_location(program, "u");
		gen_buffers(1, &buffer);
		bind_buffer(GL_ARRAY_BUFFER, buffer);
		buffer_data(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
		enable_vertex_attrib_array(0);
		vertex_attrib_pointer(0, 2, GL_FLOAT, 0, 0, NULL);

		mach_timebase_info(&timebase);
		for (frame = 0; frame < 6; frame++)
		{
			clear(GL_COLOR_BUFFER_BIT);
			start = mach_absolute_time();
			for (count = 0; count < draws; count++)
			{
				uniform4f(uniform, 1.0f, 1.0f, (float)(count % 100) / 50.0f - 1.0f, (float)(count / 100) / 100.0f - 1.0f);
				draw_arrays(GL_TRIANGLES, 0, 3);
			}
			end = mach_absolute_time();
			finish();
			if (frame > 0)
				printf("frame %d: %d draws, each with a uniform change: %.0f ns of CPU per draw (submit), %.1f ms with glFinish\n",
					frame, draws, (double)(end - start) * timebase.numer / timebase.denom / draws,
					(double)(mach_absolute_time() - start) * timebase.numer / timebase.denom / 1e6);
		}
	}
	return 0;
}
