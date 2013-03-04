/*
Copyright (c) 2012, Broadcom Europe Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	 * Redistributions of source code must retain the above copyright
		notice, this list of conditions and the following disclaimer.
	 * Redistributions in binary form must reproduce the above copyright
		notice, this list of conditions and the following disclaimer in the
		documentation and/or other materials provided with the distribution.
	 * Neither the name of the copyright holder nor the
		names of its contributors may be used to endorse or promote products
		derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// OpenGL|ES 2 UV Mapper

#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>

#include "bcm_host.h"
#include "png.h"

#include "GLES2/gl2.h"
#include "EGL/egl.h"
#include "EGL/eglext.h"

typedef struct
{
	char* filename;
	bool loop;
	void* egl_image;
} VIDEO_INFO;

typedef struct
{
	bool verbose;
	
	uint32_t screen_width;
	uint32_t screen_height;
	
	// OpenGL|ES objects
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;

	GLuint program;
	GLuint texture[3];
	GLuint vertex_buffer;

	// shader attribs
	GLuint attrib_vertex;
	GLuint uniform_mapMsb, uniform_mapLsb;
	GLuint uniform_source;

	// video texture
	void* egl_image;
	pthread_t video_thread;
	
	int frame_available;
} APP_STATE_T;
static APP_STATE_T _state, *state=&_state;


// forward declaration
void* video_decode(void* arg);
int video_decode_dimensions(char *filename, int *frame_width, int *frame_height);



#define checkgl() assert(glGetError() == 0)

static void show_shaderlog(GLint shader)
{
	// Prints the compile log for a shader
	char log[1024];
	glGetShaderInfoLog(shader,sizeof log,NULL,log);
	if (log[0]!=0)
		printf("Shader (%d): %s\n", shader, log);
}

static void show_programlog(GLint shader)
{
	// Prints the information log for a program object
	char log[1024];
	glGetProgramInfoLog(shader,sizeof log,NULL,log);
	if (log[0]!=0)
		printf("Program (%d): %s\n", shader, log);
}

/***********************************************************
 * Name: init_ogl
 *
 * Description: Sets the display, OpenGL|ES context and screen stuff
 *
 * Returns: void
 *
 ***********************************************************/
static void init_ogl()
{
	int32_t success = 0;
	EGLBoolean result;
	EGLint num_config;

	static EGL_DISPMANX_WINDOW_T nativewindow;

	DISPMANX_ELEMENT_HANDLE_T dispman_element;
	DISPMANX_DISPLAY_HANDLE_T dispman_display;
	DISPMANX_UPDATE_HANDLE_T dispman_update;
	VC_RECT_T dst_rect;
	VC_RECT_T src_rect;

	static const EGLint attribute_list[] =
	{
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_NONE
	};

	static const EGLint context_attributes[] =
	{
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLConfig config;

	// get an EGL display connection
	state->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	assert(state->display!=EGL_NO_DISPLAY);
	checkgl();

	// initialize the EGL display connection
	result = eglInitialize(state->display, NULL, NULL);
	assert(EGL_FALSE != result);
	checkgl();

	// get an appropriate EGL frame buffer configuration
	result = eglChooseConfig(state->display, attribute_list,
		&config, 1, &num_config);
	assert(EGL_FALSE != result);
	checkgl();

	// get an appropriate EGL frame buffer configuration
	result = eglBindAPI(EGL_OPENGL_ES_API);
	assert(EGL_FALSE != result);
	checkgl();

	// create an EGL rendering context
	state->context = eglCreateContext(state->display,
		config, EGL_NO_CONTEXT, context_attributes);
	assert(state->context!=EGL_NO_CONTEXT);
	checkgl();

	// create an EGL window surface
	success = graphics_get_display_size(0 /* LCD */,
		&state->screen_width, &state->screen_height);
	assert( success >= 0 );

	dst_rect.x = 0;
	dst_rect.y = 0;
	dst_rect.width = state->screen_width;
	dst_rect.height = state->screen_height;

	src_rect.x = 0;
	src_rect.y = 0;
	src_rect.width = state->screen_width << 16;
	src_rect.height = state->screen_height << 16;

	dispman_display = vc_dispmanx_display_open( 0 /* LCD */);
	dispman_update = vc_dispmanx_update_start( 0 );

	dispman_element = vc_dispmanx_element_add ( dispman_update, dispman_display,
		0/*layer*/, &dst_rect, 0/*src*/,
		&src_rect, DISPMANX_PROTECTION_NONE, 0 /*alpha*/, 0/*clamp*/, 0/*transform*/);

	nativewindow.element = dispman_element;
	nativewindow.width = state->screen_width;
	nativewindow.height = state->screen_height;
	vc_dispmanx_update_submit_sync( dispman_update );

	checkgl();

	state->surface = eglCreateWindowSurface( state->display,
		config, &nativewindow, NULL );
	assert(state->surface != EGL_NO_SURFACE);
	checkgl();

	// connect the context to the surface
	result = eglMakeCurrent(state->display, state->surface,
		state->surface, state->context);
	assert(EGL_FALSE != result);
	checkgl();

	// Set background color and clear buffers
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear( GL_COLOR_BUFFER_BIT );
	checkgl();
	
	// create 3 EGL texture surfaces; 2 map textures and a video texture
	glGenTextures(3, &state->texture[0]);
	checkgl();
}


static void init_shaders()
{
	static const GLfloat vertex_data[] = {
		-1.0,-1.0, 1.0, 1.0,
		 1.0,-1.0, 1.0, 1.0,
		 1.0, 1.0, 1.0, 1.0,
		-1.0, 1.0, 1.0, 1.0
	};
	const GLchar *vshader_source =
		"attribute vec4 vertex;"
		"varying vec2 tcoord;"
		"void main(void) {"
		"  gl_Position = vertex;"
		"  tcoord = vertex.xy*0.5+0.5;"
		"}";

	// UV Mapping fragment shader, flips source vertically
	const GLchar *fshader_source =
		"varying vec2 tcoord;"
		"uniform sampler2D mapMsb;"
		"uniform sampler2D mapLsb;"
		"uniform sampler2D source;"
		"void main(void) {"
		"  vec4 uv = texture2D(mapMsb,tcoord) + texture2D(mapLsb,tcoord)/256.;"
		"  uv.g = 1.0 - uv.g;"
		"  gl_FragColor.rgb = texture2D(source, uv.xy).rgb;"
		"  gl_FragColor.a = uv.a;"
		"}";

	GLuint vshader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vshader, 1, &vshader_source, 0);
	glCompileShader(vshader);
	checkgl();

	if (state->verbose)
		 show_shaderlog(vshader);

	GLuint fshader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fshader, 1, &fshader_source, 0);
	glCompileShader(fshader);
	checkgl();

	if (state->verbose)
		 show_shaderlog(fshader);

	state->program = glCreateProgram();
	glAttachShader(state->program, vshader);
	glAttachShader(state->program, fshader);
	glLinkProgram(state->program);
	checkgl();

	if (state->verbose)
		 show_programlog(state->program);

	state->attrib_vertex  = glGetAttribLocation(state->program, "vertex");
	state->uniform_mapMsb = glGetUniformLocation(state->program, "mapMsb");
	state->uniform_mapLsb = glGetUniformLocation(state->program, "mapLsb");
	state->uniform_source = glGetUniformLocation(state->program, "source");
	checkgl();

	glClearColor ( 0.0, 0.0, 0.0, 1.0 );

	glGenBuffers(1, &state->vertex_buffer);
	checkgl();

	// Prepare viewport
	glViewport (0, 0, state->screen_width, state->screen_height);
	checkgl();

	// Upload vertex data to a buffer
	glBindBuffer(GL_ARRAY_BUFFER, state->vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data),
								vertex_data, GL_STATIC_DRAW);
	glVertexAttribPointer(state->attrib_vertex, 4, GL_FLOAT, 0, 16, 0);
	glEnableVertexAttribArray(state->attrib_vertex);
	checkgl();
}


static int load_map(const char * file_name)
{
	png_byte header[8];

	if (state->verbose)
		printf("Loading map\n");		
	
	FILE *fp = fopen(file_name, "rb");
	if (fp == 0)
	{
		perror(file_name);
		return -1;
	}

	// read the header
	fread(header, 1, 8, fp);

	if (png_sig_cmp(header, 0, 8))
	{
		printf("error: %s is not a PNG.\n", file_name);
		fclose(fp);
		return -1;
	}

	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr)
	{
		printf("error: png_create_read_struct returned 0.\n");
		fclose(fp);
		return -1;
	}

	// create png info struct
	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		printf("error: png_create_info_struct returned 0.\n");
		png_destroy_read_struct(&png_ptr, (png_infopp)NULL, (png_infopp)NULL);
		fclose(fp);
		return -1;
	}

	// create png info struct
	png_infop end_info = png_create_info_struct(png_ptr);
	if (!end_info)
	{
		printf("error: png_create_info_struct returned 0.\n");
		png_destroy_read_struct(&png_ptr, &info_ptr, (png_infopp) NULL);
		fclose(fp);
		return -1;
	}

	// the code in this if statement gets called if libpng encounters an error
	if (setjmp(png_jmpbuf(png_ptr))) {
		printf("error from libpng\n");
		png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
		fclose(fp);
		return -1;
	}

	// init png reading
	png_init_io(png_ptr, fp);

	// let libpng know you already read the first 8 bytes
	png_set_sig_bytes(png_ptr, 8);

	// read all the info up to the image data
	png_read_info(png_ptr, info_ptr);

	// variables to pass to get info
	int bit_depth, color_type;
	png_uint_32 temp_width, temp_height;

	// get info about png
	png_get_IHDR(png_ptr, info_ptr, &temp_width, &temp_height, &bit_depth, &color_type,
		NULL, NULL, NULL);
		
	if(bit_depth!=16)
	{
		printf("error: expected 16 bit per channel map\n");
		return -1;
	}
	if(color_type!=PNG_COLOR_TYPE_RGB_ALPHA)
	{
		printf("error: expected RGBA map\n");
		return -1;
	}

	// Update the png info struct.
	png_read_update_info(png_ptr, info_ptr);

	// Row size in bytes.
	int rowbytes = png_get_rowbytes(png_ptr, info_ptr);

	// glTexImage2d requires rows to be 4-byte aligned
	rowbytes += 3 - ((rowbytes-1) % 4);

	// Allocate the image_data as a big block, to be given to opengl
	png_byte * image_data;
	image_data = malloc(rowbytes * temp_height * sizeof(png_byte)+15);
	if (image_data == NULL)
	{
		printf("error: could not allocate memory for PNG image data\n");
		png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
		fclose(fp);
		return -1;
	}

	// row_pointers is for pointing to image_data for reading the png with libpng
	png_bytep * row_pointers = malloc(temp_height * sizeof(png_bytep));
	if (row_pointers == NULL)
	{
		printf("error: could not allocate memory for PNG row pointers\n");
		png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
		free(image_data);
		fclose(fp);
		return -1;
	}

	// set the individual row_pointers to point at the correct offsets of image_data
	int i;
	for (i = 0; i < temp_height; i++)
	{
		row_pointers[temp_height - 1 - i] = image_data + i * rowbytes;
	}


	if (state->verbose)
		printf("Splitting map\n");	

	// read the png into image_data through row_pointers
	png_read_image(png_ptr, row_pointers);
	
	// split 16 image into two 8 bit imagebuffers
	int map_sz = rowbytes * temp_height / 2;
	char* texture_buffer1 = malloc(map_sz);
	char* texture_buffer2 = malloc(map_sz);

	for(i = 0; i < map_sz; i++)
	{
		texture_buffer1[i] = image_data[2*i];
		texture_buffer2[i] = image_data[2*i+1];
	}

	// first texture (map msb)
	glBindTexture(GL_TEXTURE_2D, state->texture[0]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, temp_width, temp_height, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, texture_buffer1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	checkgl();

	// second texture (map lsb)
	glBindTexture(GL_TEXTURE_2D, state->texture[1]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, temp_width, temp_height, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, texture_buffer2);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	checkgl();
	
	// release texture buffers
	free(texture_buffer1);
	free(texture_buffer2);	
	
	// clean up
	png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
	free(image_data);
	free(row_pointers);
	fclose(fp);
	
	return 0;
}


static int make_video_texture(int video_width, int video_height)
{
	// setup texture for video
	int image_size = video_width * video_height * 4;
	GLubyte* image_buffer = malloc(image_size);
	if (image_buffer == 0)
	{
		printf("error: could not allocate memory for video buffer.\n");
		return -1;
	}

	memset(image_buffer, 0x00, image_size);  // black transparant

	glBindTexture(GL_TEXTURE_2D, state->texture[2]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, video_width, video_height, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, image_buffer);

	free(image_buffer);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	
	// Create EGL Image
	state->egl_image = 0;
	state->egl_image = eglCreateImageKHR(
					 state->display,
					 state->context,
					 EGL_GL_TEXTURE_2D_KHR,
					 (EGLClientBuffer)state->texture[2],
					 0);
	 
	if (state->egl_image == EGL_NO_IMAGE_KHR)
	{
		printf("error: eglCreateImageKHR failed.\n");
		return -1;
	}

	if (state->verbose)
		printf("EGL image created = %x\n", (uint)state->egl_image);
		
	return 0;
}

static void init_textures(char *map_filename, char *video_filename)
{
	if(load_map(map_filename)<0)
	{
		exit(-1);
	}

	int frame_width, frame_height;
	if(video_decode_dimensions(video_filename, &frame_width, &frame_height)<0)
	{
		printf("error: could not get video dimensions.\n");
		exit(-1);		
	}
	if (state->verbose)
		printf("Video dimensions: %d x %d\n", frame_width, frame_height);
		
	if(make_video_texture(frame_width, frame_height)<0)
	{
		exit(-1);
	}
}

static void start_rendering(char *video_filename, bool loop)
{
	VIDEO_INFO video_info;
	memset( &video_info, 0, sizeof( video_info ) );
	video_info.filename = video_filename;
	video_info.loop = loop;
	video_info.egl_image = state->egl_image;
	
	// Start rendering
	pthread_create(&state->video_thread, NULL, video_decode, &video_info);	

	if (state->verbose)
		printf("Video thread created\n");
}

static void draw_triangles()
{
	// Render to the main frame buffer
	glBindFramebuffer(GL_FRAMEBUFFER,0);

	// Clear the background
	glClear(GL_COLOR_BUFFER_BIT);
	checkgl();

	glBindBuffer(GL_ARRAY_BUFFER, state->vertex_buffer);
	checkgl();
	glUseProgram ( state->program );
	checkgl();
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D,state->texture[0]);
	checkgl();
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D,state->texture[1]);
	checkgl();
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D,state->texture[2]);
	checkgl();

	glUniform1i(state->uniform_mapMsb, 0);
	checkgl();
	glUniform1i(state->uniform_mapLsb, 1);
	checkgl();
	glUniform1i(state->uniform_source, 2);
	checkgl();

	glDrawArrays ( GL_TRIANGLE_FAN, 0, 4 );
	checkgl();

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glFlush();
	glFinish();
	checkgl();

	eglSwapBuffers(state->display, state->surface);
	checkgl();
}

void set_frame_available()
{
	state->frame_available = 1;
}

static void cleanup(void)
// Clean up resources
{
	pthread_cancel(state->video_thread);

	if (state->egl_image != 0)
	{
		if (!eglDestroyImageKHR(state->display, (EGLImageKHR) state->egl_image))
			printf("eglDestroyImageKHR failed.");
	}

	// clear screen
	glClear( GL_COLOR_BUFFER_BIT );
	eglSwapBuffers(state->display, state->surface);

	// Release OpenGL resources
	eglMakeCurrent( state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
	eglDestroySurface( state->display, state->surface );
	eglDestroyContext( state->display, state->context );
	eglTerminate( state->display );

	if (state->verbose)
		printf("App closed\n");
}

//==============================================================================

int main (int argc, char **argv)
{
	int terminate = 0;
	
	atexit(cleanup);
	bcm_host_init();
	
	// Clear application state
	memset( state, 0, sizeof( *state ) );

	if (argc < 3) {
		printf("Usage: %s [OPTION] <mapfile> <moviefile>\n", argv[0]);
		printf("  -l, --loop						Loop playback forever\n");
		printf("  -v, --verbose						Show debug information\n");
		exit(1);
	}
	
	bool loop = false;
	int c;
	for(c=1; c<argc-1; c++) {
		if (strcmp(argv[c],"-l")==0 || strcmp(argv[c],"--loop") == 0)
			loop = true;
		if (strcmp(argv[c],"-v")==0 || strcmp(argv[c],"--verbose") == 0)
			state->verbose = true;
	}
		
	// Start OGLES
	init_ogl();
	init_textures(argv[argc-2], argv[argc-1]);
	init_shaders();
	
	start_rendering(argv[argc-1], loop);

	while (!terminate)
	{
		if (state->frame_available)
		{
			state->frame_available = 0;
			draw_triangles();
		}
	}

	return terminate;
}
