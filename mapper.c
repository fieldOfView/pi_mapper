
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
#include <termios.h>
#include <poll.h>
#include <assert.h>
#include <pthread.h>

#include "bcm_host.h"

#include "GLES2/gl2.h"
#include "EGL/egl.h"
#include "EGL/eglext.h"

#define IMAGE_PATH "test.raw"
#define IMAGE_WIDTH 1920
#define IMAGE_HEIGHT 1080

#define MAP_PATH "map.raw"
#define MAP_WIDTH 1680
#define MAP_HEIGHT 1050

typedef struct
{
	int verbose;
	
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
	
	// pointers to texture buffers
	char *texture_buffer1;
	char *texture_buffer2;
	char *texture_buffer3;

	// video texture
	void* egl_image;
	pthread_t video_thread;
} APP_STATE_T;

static APP_STATE_T _state, *state=&_state;
static struct termios last_term, my_term;

static int frame_available = 0;

// forward declaration
void* video_decode_test(void* arg);



#define checkgl() assert(glGetError() == 0)

static void show_shaderlog(GLint shader)
{
	// Prints the compile log for a shader
	char log[1024];
	glGetShaderInfoLog(shader,sizeof log,NULL,log);
	printf("Shader (%d): %s\n", shader, log);
}

static void show_programlog(GLint shader)
{
	// Prints the information log for a program object
	char log[1024];
	glGetProgramInfoLog(shader,sizeof log,NULL,log);
	printf("Program (%d): %s\n", shader, log);
}

/***********************************************************
 * Name: init_ogl
 *
 * Arguments:
 *		 APP_STATE_T *state - holds OGLES model info
 *
 * Description: Sets the display, OpenGL|ES context and screen stuff
 *
 * Returns: void
 *
 ***********************************************************/
static void init_ogl(APP_STATE_T *state)
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
	glClearColor(0.15f, 0.25f, 0.35f, 1.0f);
	glClear( GL_COLOR_BUFFER_BIT );

	checkgl();
}


static void init_shaders(APP_STATE_T *state)
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
		"  tcoord.y = 1.0 - tcoord.y;"
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


static void load_images(APP_STATE_T *state)
{
	FILE *file = NULL;
	int bytes_read, 
		image_sz = IMAGE_WIDTH*IMAGE_HEIGHT*3,
		map_sz = MAP_WIDTH*MAP_HEIGHT*4;

	char *map_buffer = malloc(map_sz*2);
	state->texture_buffer1 = malloc(map_sz);
	state->texture_buffer2 = malloc(map_sz);
	state->texture_buffer3 = malloc(image_sz);

	file = fopen(MAP_PATH, "rb");
	if (file && map_buffer)
	{
		bytes_read=fread(map_buffer, 1, map_sz*2, file);
		assert(bytes_read == map_sz*2);  // some problem with file?
		fclose(file);

		if (state->verbose)
			printf("Map loaded\n");

	  int i;
		for(i = 0; i < map_sz; i++)
		{
			state->texture_buffer1[i] = map_buffer[2*i];
			state->texture_buffer2[i] = map_buffer[2*i+1];
		}
		if (state->verbose)
			printf("Map split\n");
	}

	free(map_buffer);
}


static void init_textures(APP_STATE_T *state)
{
	// load texture buffers into OGL|ES texture surfaces
	load_images(state);
	glGenTextures(3, &state->texture[0]);
	checkgl();

	// first texture (map msb)
	glBindTexture(GL_TEXTURE_2D, state->texture[0]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MAP_WIDTH, MAP_HEIGHT, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, state->texture_buffer1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	checkgl();

	// second texture (map lsb)
	glBindTexture(GL_TEXTURE_2D, state->texture[1]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MAP_WIDTH, MAP_HEIGHT, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, state->texture_buffer2);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	checkgl();

	// setup texture for video
	int image_size = IMAGE_WIDTH * IMAGE_HEIGHT * 4;
	GLubyte* image_buffer = malloc(image_size);
	if (image_buffer == 0)
	{
		printf("malloc failed.\n");
		exit(1);
	}

	memset(image_buffer, 0x00, image_size);  // black transparant

	glBindTexture(GL_TEXTURE_2D, state->texture[2]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, IMAGE_WIDTH, IMAGE_HEIGHT, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, image_buffer);

	free(image_buffer);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);	
	
	/* Create EGL Image */
	state->egl_image = 0;
	state->egl_image = eglCreateImageKHR(
					 state->display,
					 state->context,
					 EGL_GL_TEXTURE_2D_KHR,
					 (EGLClientBuffer)state->texture[2],
					 0);
	 
	if (state->egl_image == EGL_NO_IMAGE_KHR)
	{
		printf("eglCreateImageKHR failed.\n");
		exit(1);
	}
	else
	{
		if (state->verbose)
			printf("EGL image created = %x\n", (uint)state->egl_image);
	}	

	// Start rendering
	pthread_create(&state->video_thread, NULL, video_decode_test, state->egl_image);	

	if (state->verbose)
		printf("Textures ready\n");
}

static void draw_triangles(APP_STATE_T *state)
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
	frame_available = 1;
}

void init_termios(int echo)
// Initialize new terminal i/o settings
{
	tcgetattr(0, &last_term);					 // store terminal i/o settings
	my_term = last_term;
	my_term.c_lflag &= ~ICANON;				  // disable buffered i/o
	my_term.c_lflag &= echo ? ECHO : ~ECHO;  // set echo mode
	tcsetattr(0, TCSANOW, &my_term);
}

void reset_termios(void)
// Restore last_term terminal i/o settings
{
	tcsetattr(0, TCSANOW, &last_term);
}

char check_keypress(void)
{
	struct pollfd pfds[1];
	char c;
	int ret;

	// See if there is data available
	pfds[0].fd = 0;
	pfds[0].events = POLLIN;
	ret = poll(pfds, 1, 0);

	if (ret > 0)
	{
		if (state->verbose)
			printf("Key pressed\n");

		// Consume data
		read(0, &c, 1);
	  return c;
	}

	return 0;
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

	// release texture buffers
	free(state->texture_buffer1);
	free(state->texture_buffer2);
	free(state->texture_buffer3);

	//reset_termios();

	if (state->verbose)
		printf("App closed\n");
}

//==============================================================================

int main ()
{
	int terminate = 0;
	bcm_host_init();

	// Clear application state
	memset( state, 0, sizeof( *state ) );
	state->verbose = 1;

	// Start OGLES
	init_ogl(state);
	init_shaders(state);
	init_textures(state);

	//init_termios(0);

	while (!terminate)
	{
		if (frame_available)
		{
			frame_available = 0;
			draw_triangles(state);
		}
		
		//if (check_keypress())
		//	terminate = 1;
	}
	cleanup();

	return terminate;
}
