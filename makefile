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
              $(BUILDDIR)/worker.o $(BUILDDIR)/jobs.o
ADMIN_OBJS  = $(BUILDDIR)/admin.o $(BUILDDIR)/proto.o
REMOTE_OBJS = $(BUILDDIR)/remote.o $(BUILDDIR)/proto.o
REST_OBJS   = $(BUILDDIR)/rest_server.o $(BUILDDIR)/video_ops.o \
			  $(BUILDDIR)/worker.o

SERVER_BIN  = $(BINDIR)/vps_server
ADMIN_BIN   = $(BINDIR)/vps_admin
REMOTE_BIN  = $(BINDIR)/vps_remote
REST_BIN    = $(BINDIR)/vps_rest

# Compat: vps_client e alias catre vps_admin (clientii vechi)
CLIENT_BIN  = $(BINDIR)/vps_client

.PHONY: all clean tidy help dirs server client rest asan tsan wstest pyclient capture

all: dirs $(SERVER_BIN) $(ADMIN_BIN) $(REMOTE_BIN) $(REST_BIN)

# Sanitizer builds per cerinta grila notare:
#   ASan/LSan/UBSan (mandatory) si TSan (optional, B/C).
# ASan si TSan nu pot fi active simultan -> build-uri separate.
ASAN_FLAGS = -O0 -fno-omit-frame-pointer -fsanitize=address,leak,undefined
TSAN_FLAGS = -O0 -fno-omit-frame-pointer -fsanitize=thread -pthread

asan:
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) \
		$(SRCDIR)/server.c $(SRCDIR)/net_server.c $(SRCDIR)/proto.c \
		$(SRCDIR)/video_ops.c $(SRCDIR)/worker.c $(SRCDIR)/jobs.c \
		-o $(BINDIR)/vps_server_asan $(LIBCONFIG) $(LIBAV)
	$(CC) $(CFLAGS) $(ASAN_FLAGS) \
		$(SRCDIR)/admin.c $(SRCDIR)/proto.c \
		-o $(BINDIR)/vps_admin_asan
	$(CC) $(CFLAGS) $(ASAN_FLAGS) \
		$(SRCDIR)/remote.c $(SRCDIR)/proto.c \
		-o $(BINDIR)/vps_remote_asan
	$(CC) $(CFLAGS) $(ASAN_FLAGS) \
		$(SRCDIR)/rest_server.c $(SRCDIR)/video_ops.c $(SRCDIR)/worker.c \
		-o $(BINDIR)/vps_rest_asan $(LIBCONFIG) $(LIBAV)

tsan:
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(TSAN_FLAGS) \
		$(SRCDIR)/server.c $(SRCDIR)/net_server.c $(SRCDIR)/proto.c \
		$(SRCDIR)/video_ops.c $(SRCDIR)/worker.c $(SRCDIR)/jobs.c \
		-o $(BINDIR)/vps_server_tsan $(LIBCONFIG) $(LIBAV)
	$(CC) $(CFLAGS) $(TSAN_FLAGS) \
		$(SRCDIR)/rest_server.c $(SRCDIR)/video_ops.c $(SRCDIR)/worker.c \
		-o $(BINDIR)/vps_rest_tsan $(LIBCONFIG) $(LIBAV)

help:
	@echo "Targets:"
	@echo "  make all     -- build server and client"
	@echo "  make server  -- build the video processing server"
	@echo "  make client  -- build the test client"
	@echo "  make rest    -- build the REST shim server"
	@echo "  make asan    -- build server/client/rest with ASan+LSan+UBSan"
	@echo "  make tsan    -- build server/rest with ThreadSanitizer"
	@echo "  make tidy    -- run clang-tidy on all sources"
	@echo "  make clean   -- remove all build artefacts"
	@echo ""
	@echo "Sanitizers / memory tools (cf. cerinta grila notare):"
	@echo "  ASan/LSan/UBSan: ./bin/vps_server_asan -p 19090"
	@echo "  ThreadSanitizer: ./bin/vps_server_tsan -p 19091"
	@echo "  Valgrind memcheck:"
	@echo "    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./bin/vps_server"
	@echo "  Valgrind helgrind:"
	@echo "    valgrind --tool=helgrind ./bin/vps_server"
	@echo ""
	@echo "Dependencies:"
	@echo "  sudo apt install libconfig-dev valgrind"
	@echo "  sudo apt install libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev ffmpeg"

dirs:
	@mkdir -p $(BUILDDIR) $(BINDIR)

server: dirs $(SERVER_BIN)
admin:  dirs $(ADMIN_BIN)
remote: dirs $(REMOTE_BIN)
rest:   dirs $(REST_BIN)

$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBCONFIG) $(LIBAV)

$(ADMIN_BIN): $(ADMIN_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(REMOTE_BIN): $(REMOTE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(REST_BIN): $(REST_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBCONFIG) $(LIBAV)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

tidy:
	clang-tidy $(SRCDIR)/*.c -- $(CFLAGS)

# Milestone 3: client OTH (Python) + tester REST + captura Wireshark
pyclient:
	@python3 -m py_compile clients/python/vps_remote.py && echo "pyclient ok"

wstest:
	python3 clients/python/vps_rest_test.py --base http://127.0.0.1:18082

capture:
	./scripts/wireshark_capture.sh

clean:
	rm -rf $(BUILDDIR) $(BINDIR)
