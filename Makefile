# Linux / WSL: make test        Windows (MSYS2 gcc): mingw32-make test
CC      = gcc
BUILD   = build
SAN     =
CFLAGS  = -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Wpedantic \
          -pthread -Iinclude -Ibench -Iui -MMD -MP $(SAN)
LDFLAGS = -pthread $(SAN)

ifeq ($(OS),Windows_NT)
  SHELL    := cmd.exe
  EXE      := .exe
  LDFLAGS  += -static
  UI_LIBS   = -lws2_32 -mwindows
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
PROGS = $(BUILD)/test$(EXE) $(BUILD)/test_sim$(EXE) $(BUILD)/bench$(EXE) \
        $(BUILD)/demo$(EXE) $(BUILD)/dashboard$(EXE)

vpath %.c src tests bench examples ui

all: $(LIB) $(PROGS)

$(LIB): $(BUILD)/bqueue.o $(BUILD)/threadpool.o
	ar rcs $@ $^

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# Programs that need more than their own source and the library
$(BUILD)/bench$(EXE): $(BUILD)/strategies.o
$(BUILD)/test_sim$(EXE): $(BUILD)/sim.o $(BUILD)/strategies.o
$(BUILD)/dashboard$(EXE): $(BUILD)/sim.o $(BUILD)/strategies.o
$(BUILD)/dashboard$(EXE): LIBS = $(UI_LIBS)

$(PROGS): $(BUILD)/%$(EXE): $(BUILD)/%.o $(LIB)
	$(CC) -o $@ $(filter %.o,$^) $(LIB) $(LDFLAGS) $(LIBS)

$(BUILD) results:
	$(call mkdir,$@)

test: $(BUILD)/test$(EXE) $(BUILD)/test_sim$(EXE)
	$(RUNNER) $(call run,$(BUILD)/test$(EXE))
	$(RUNNER) $(call run,$(BUILD)/test_sim$(EXE))

# The dashboard runs build/test for its Tests tab.
ui: $(BUILD)/dashboard$(EXE) $(BUILD)/test$(EXE)
	$(call run,$<)

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
.PHONY: all test ui demo bench tsan asan clean
