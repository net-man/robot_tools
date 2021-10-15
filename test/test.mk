CFLAGS_TEST		=	-Wall -pedantic -std=c11 -target x86_64-pc-windows-gnu -Iinclude -Lbin -g -O0
LIBS_TEST		= 	-lm -lpthread					\
					-lsetupapi						\
					-lhid							\
					-lhidparse						\
					-lcfgmgr32						\
					-linpt

SRC			   := $(wildcard test/*.c)

.PHONY: test

test:
	$(CC) $(CFLAGS_TEST) $(SRC) $(LIBS_TEST) -o bin/test.exe