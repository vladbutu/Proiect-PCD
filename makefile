# Makefile for T5 Video Processing server (Level A: poll/select + fork/exec)
# Mandatory flags per assignment requirements.

CC        = gcc
CSTD      = -std=c11
CDEFS     = -D_POSIX_C_SOURCE=200809L
CWARN     = -Wall -Wextra -Wpedantic -Werror
CDEBUG    = -g
CFLAGS    = $(CSTD) $(CDEFS) $(CWARN) $(CDEBUG) -Iinclude

# External libraries:
#   libconfig  -- mandatory configuration file parsing
#   libav*     -- mandatory external lib (FFmpeg) for video probing
LIBCONFIG = $(shell pkg-config --cflags --libs libconfig 2>/dev/null || echo -lconfig)
LIBAV     = $(shell pkg-config --cflags --libs libavformat libavcodec libavutil libavfilter 2>/dev/null \
                || echo -lavformat -lavcodec -lavutil -lavfilter)

SRCDIR    = src
BUILDDIR  = build
BINDIR    = bin

SERVER_OBJS = $(BUILDDIR)/server.o $(BUILDDIR)/net_server.o \
              $(BUILDDIR)/proto.o  $(BUILDDIR)/video_ops.o  \
              $(BUILDDIR)/worker.o
CLIENT_OBJS = $(BUILDDIR)/client.o $(BUILDDIR)/proto.o
REST_OBJS   = $(BUILDDIR)/rest_server.o $(BUILDDIR)/video_ops.o \
			  $(BUILDDIR)/worker.o

SERVER_BIN  = $(BINDIR)/vps_server
CLIENT_BIN  = $(BINDIR)/vps_client
REST_BIN    = $(BINDIR)/vps_rest

.PHONY: all clean tidy help dirs server client rest

all: dirs $(SERVER_BIN) $(CLIENT_BIN) $(REST_BIN)

help:
	@echo "Targets:"
	@echo "  make all     -- build server and client"
	@echo "  make server  -- build the video processing server"
	@echo "  make client  -- build the test client"
	@echo "  make rest    -- build the REST shim server"
	@echo "  make tidy    -- run clang-tidy on all sources"
	@echo "  make clean   -- remove all build artefacts"
	@echo ""
	@echo "Dependencies:"
	@echo "  sudo apt install libconfig-dev"
	@echo "  sudo apt install libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev ffmpeg"

dirs:
	@mkdir -p $(BUILDDIR) $(BINDIR)

server: dirs $(SERVER_BIN)
client: dirs $(CLIENT_BIN)
rest: dirs $(REST_BIN)

$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBCONFIG) $(LIBAV)

$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(REST_BIN): $(REST_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBCONFIG) $(LIBAV)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

tidy:
	clang-tidy $(SRCDIR)/*.c -- $(CFLAGS)

clean:
	rm -rf $(BUILDDIR) $(BINDIR)
