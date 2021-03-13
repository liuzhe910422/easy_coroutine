.PHONY: clean
CC = gcc
RM = rm
EXE = example
FLAG = -lpthread
SRCS = $(wildcard *.c)
OBJS = $(patsubst %.c,%.o,$(SRCS))
$(EXE): $(OBJS)
	$(CC) -o $@ $^ $(FLAG)
%.o: %.c
	$(CC) -o $@ -c $^
clean:
	$(RM) $(EXE) $(OBJS)