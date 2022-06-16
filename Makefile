LIBS =
INC = -I./include
FLAGS = -Wall -O3
SRC = $(wildcard src/*.c)
OBJ = $(patsubst src/%.c,obj/%.o,$(SRC))

CC = gcc
BIN = main server echo

all:obj $(BIN)

obj:
	@mkdir -p $@

main:main.o coroutine.o timer.o  btree.o
	$(CC) -o $@ $^ $(LIBS)

server:server.o coroutine.o timer.o btree.o
	$(CC) -o $@ $^ $(LIBS)

echo:echo.o
	$(CC) -o $@ $^ $(LIBS)

$(OBJ):obj/%.o:src/%.c
	$(CC) -c $(FLAGS) -o $@ $< $(INC)

clean:
	-rm $(BIN) $(OBJ)
	@rmdir obj

.PHONY:all clean

vpath %.c src
vpath %.o obj
vpath %.h include
