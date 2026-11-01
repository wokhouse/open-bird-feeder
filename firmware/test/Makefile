CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -I../main
SRC := zone_motion_test.c ../main/zone_motion.c
BIN := zone_motion_test

# Host build of the firmware's zone detector. No ESP-IDF, no hardware.
# ../build is the idf.py output dir, so keep the binary out of CMake's way by
# building it inside this directory only (CMake only compiles main/).
all: $(BIN)

$(BIN): $(SRC) ../main/zone_motion.h ../main/zone_config.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all run clean
