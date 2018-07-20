CC=gcc
LDFLAGS=-lavformat -lavcodec -lswscale -lavutil -lz -lSDL2
CFLAGS=-g -Wall

SOURCES=main.c logging.c
EXECUTABLE=player

all: $(EXECUTABLE) 

$(EXECUTABLE): $(SOURCES)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SOURCES) -o $@

clean:
	rm $(EXECUTABLE)
