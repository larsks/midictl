LIBPIPEWIRE=libpipewire-0.3

CFLAGS=-Wall $(shell pkg-config --cflags $(LIBPIPEWIRE))
LIBS=$(shell pkg-config --libs $(LIBPIPEWIRE))

all: midictl

midictl: midictl.o
	gcc -o $@ $^ $(LIBS)

clean:
	rm -f midictl.o midictl
