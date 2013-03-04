pi_mapper
=========

Implementation of a uv-mapping video processor for the Raspberry Pi.

*Usage:* ./uvmapper.bin [OPTION] <mapfile> <moviefile>
  -l, --loop                                            Loop playback forever
  -v, --verbose                                         Show debug information


The source is based on the Raspberry Pi sample code, and references its Makefile.include:
https://github.com/raspberrypi/firmware/tree/master/opt/vc/src/hello_pi/hello_triangle2
https://github.com/raspberrypi/firmware/tree/master/opt/vc/src/hello_pi/hello_video

Video rendering to OpenGL|ES texture by @OtherCrashOverride:
https://github.com/OtherCrashOverride/hello_videocube