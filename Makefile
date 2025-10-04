# Makefile (outputs in current dir; sources in ./src)

# --- Config ---
PKGS := gstreamer-1.0 gstreamer-app-1.0 gio-2.0
CC   ?= gcc

CFLAGS  ?= -O2 -fPIC $(shell pkg-config --cflags $(PKGS)) -Isrc
LDFLAGS ?= $(shell pkg-config --libs $(PKGS))

APP        := splash_main
LIB        := libsplashscreen.so
OBJDIR     := build

ASSET_ZIP := spinner_ai_1080p30.zip
ASSET_OUT := $(ASSET_ZIP:.zip=.h265)

# Objects
LIB_OBJS := $(OBJDIR)/splashlib.o

# --- Phony targets ---
.PHONY: all assets clean static run-udp

# Default: shared lib + app linked against it
all: assets $(LIB) $(APP)

assets: $(ASSET_OUT)

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

$(ASSET_OUT): $(ASSET_ZIP)
	@echo "Unpacking $<"
	gunzip -c $< > $@

# Convenience run targets (adjust args as needed)
run-udp: $(APP)
	./$(APP) config/demo.ini

# Cleanup
clean:
	rm -rf $(OBJDIR) $(APP) $(LIB) $(ASSET_OUT)
