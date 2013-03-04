OBJS=mapper.o video.o
BIN=uvmapper.bin
LDFLAGS+=-lilclient -lpng

include ../Makefile.include

