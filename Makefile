CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2 -g
LDFLAGS ?= -pthread
SRC      = src/main.c src/engine.c src/playlist.c src/ringbuf.c src/decoder.c src/sink.c

all: player

player: $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

# Variantes de verificación
tsan: $(SRC)
	$(CC) -std=gnu11 -Wall -Wextra -O1 -g -fsanitize=thread $(SRC) -o player_tsan -pthread
asan: $(SRC)
	$(CC) -std=gnu11 -Wall -Wextra -O1 -g -fsanitize=address,undefined $(SRC) -o player_asan -pthread

test: player
	bash tests/run_tests.sh ./player
test-tsan: tsan
	bash tests/run_tests.sh ./player_tsan
test-asan: asan
	bash tests/run_tests.sh ./player_asan

clean:
	rm -f player player_tsan player_asan tests/*.wav

.PHONY: all tsan asan test test-tsan test-asan clean
