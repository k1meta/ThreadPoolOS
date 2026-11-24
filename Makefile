# Linux / WSL: make test        Windows (MSYS2 gcc): mingw32-make test
CC      = gcc
BUILD   = build
SAN     =
CFLAGS  = -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Wpedantic \
          -pthread -Iinclude -MMD -MP $(SAN)
LDFLAGS = -pthread $(SAN)

ifeq ($(OS),Windows_NT)
  SHELL    := cmd.exe
  EXE      := .exe
  LDFLAGS  += -static
  native    = $(subst /,\,$(1))
  mkdir     = if not exist $(call native,$(1)) mkdir $(call native,$(1))
  rmdir     = if exist $(call native,$(1)) rmdir /s /q $(call native,$(1))
  run       = $(call native,$(1))
else
  EXE      :=
  mkdir     = mkdir -p $(1)
  rmdir     = rm -rf $(1)
  run       = $(abspath $(1))
endif

LIB   = $(BUILD)/libthreadpool.a
PROGS = $(BUILD)/test$(EXE) $(BUILD)/bench$(EXE) $(BUILD)/demo$(EXE)

vpath %.c src tests bench examples

all: $(LIB) $(PROGS)

$(LIB): $(BUILD)/bqueue.o $(BUILD)/threadpool.o
	ar rcs $@ $^

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(PROGS): $(BUILD)/%$(EXE): $(BUILD)/%.o $(LIB)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD) results:
	$(call mkdir,$@)

test: $(BUILD)/test$(EXE)
	$(RUNNER) $(call run,$<)

demo: $(BUILD)/demo$(EXE)
	$(call run,$<)

# Takes a few minutes. Close other programs first.
bench: $(BUILD)/bench$(EXE) | results
	$(call run,$<) > results/bench.csv

# Sanitizers are not available in MSYS2 gcc: use Linux. setarch -R turns off
# address randomization, which can stop ThreadSanitizer from starting.
tsan:
	$(MAKE) test BUILD=$(BUILD)/tsan SAN=-fsanitize=thread RUNNER="setarch -R"
asan:
	$(MAKE) test BUILD=$(BUILD)/asan SAN=-fsanitize=address,undefined

clean:
	$(call rmdir,$(BUILD))

-include $(wildcard $(BUILD)/*.d)
.PHONY: all test demo bench tsan asan clean
