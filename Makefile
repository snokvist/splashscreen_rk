# Makefile (outputs in current dir; sources in ./src)

# --- Config ---
PKGS := gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0
CC   ?= gcc

CFLAGS  ?= -O2 -fPIC $(shell pkg-config --cflags $(PKGS)) -Isrc
LDFLAGS ?= $(shell pkg-config --libs $(PKGS))

APP      := splash_main
LIB      := libsplashscreen.so
OBJDIR   := build

# Objects
LIB_OBJS := $(OBJDIR)/splashlib.o

# --- Phony targets ---
.PHONY: all clean static run-udp run-rtsp

# Default: shared lib + app linked against it
all: $(LIB) $(APP)

# Shared library
$(LIB): $(LIB_OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

# App linked against shared library in current dir (rpath=$ORIGIN)
$(APP): src/main.c $(LIB)
	$(CC) -O2 -o $@ $< -L. -lsplashscreen $(shell pkg-config --cflags $(PKGS)) $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

# Static-ish single-binary build (no .so; links the object directly)
static: $(OBJDIR)/splashlib.o
	$(CC) -O2 -o $(APP) src/main.c $^ $(shell pkg-config --cflags --libs $(PKGS))

# Pattern rule for objects in build/ from src/
$(OBJDIR)/%.o: src/%.c src/%.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# Convenience run targets (adjust args as needed)
run-udp: $(APP)
	./$(APP) spinner_1080p30.h265 30 --udp 127.0.0.1 5600

run-rtsp: $(APP)
	./$(APP) spinner_1080p30.h265 30 --rtsp 0.0.0.0 8554 /splash

# Cleanup
clean:
	rm -rf $(OBJDIR) $(APP) $(LIB)
