CFLAGS = -O2 -Wall -Wextra -std=c99 -pedantic -fvisibility=hidden
APPNAME = retris
CURTAIN = 1
ASAN = 0

OBJS = retris.o
CFLAGS += -DUSE_CURTAIN=$(CURTAIN)

ifneq ($(CURTAIN), 0)
OBJS += curtain.o
endif

ifneq ($(ASAN), 0)
CFLAGS += -fsanitize=address -static-libasan
endif

.PHONY: all clean
all: $(APPNAME)

clean:
	$(RM) $(OBJS) $(APPNAME)

curtain.o: curtain.h btgadget.h
retris.o: termgfx.h joyinput.h mstream.h joycompat.h curtain.h

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

$(APPNAME): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
