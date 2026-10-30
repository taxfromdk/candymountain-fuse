CC       ?= cc
PKGS     := fuse3 libcurl
CPPFLAGS := -Isrc -D_DEFAULT_SOURCE $(shell pkg-config --cflags $(PKGS))
CFLAGS   := -std=c11 -O2 -g -Wall -Wextra -Werror -MMD -MP -pthread
LDLIBS   := $(shell pkg-config --libs $(PKGS)) -lm

BUILD := build
BIN   := $(BUILD)/bin

# The tests need a built candymountain checkout (its ixserve and recordings).
CANDYMOUNTAIN ?= ../candymountain
TESTDATA ?= $(CANDYMOUNTAIN)/testdata
SBUILD   ?= $(CANDYMOUNTAIN)/server/build

LIB_SRCS := src/strbuf.c src/json.c src/api.c src/catalog.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libixmount.a

TOOLS     := $(BIN)/ixmount
TEST_BINS := $(BIN)/test_json

VALGRIND := valgrind --quiet --error-exitcode=1 --leak-check=full \
            --errors-for-leak-kinds=all

all: $(LIB) $(TOOLS) $(TEST_BINS)

$(BIN)/%: tools/%.c $(LIB) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LIB) $(LDLIBS)

$(BIN)/%: tests/%.c $(LIB) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LIB) $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(BUILD) $(BIN):
	mkdir -p $@

# Needs candymountain's server built, fusermount3, ffmpeg/ffprobe, curl and python3.
test: all
	$(BIN)/test_json
	$(VALGRIND) $(BIN)/test_json
	CANDYMOUNTAIN=$(CANDYMOUNTAIN) bash tests/test_mount.sh $(BUILD) $(SBUILD) $(TESTDATA)

clean:
	rm -rf $(BUILD)

-include $(BUILD)/*.d

.PHONY: all test clean
