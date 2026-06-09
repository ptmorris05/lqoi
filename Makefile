CC ?= gcc

# Third-party build-time headers (stb_image, stb_image_write) and the upstream
# lossless-QOI reference used for the size baseline are kept out of the repo in
# a gitignored vendor/ directory. run_benchmark.sh fetches them; these flags
# point the compilers at them.
VENDOR ?= vendor
PNG_CFLAGS := $(shell pkg-config --cflags libpng 2>/dev/null)
PNG_LFLAGS := $(shell pkg-config --libs libpng 2>/dev/null || echo -lpng)

CFLAGS_BENCH ?= -std=gnu99 -O3 -I$(VENDOR) -I.
LFLAGS_BENCH ?= $(PNG_LFLAGS) -lm $(LDFLAGS)
CFLAGS_CONV  ?= -std=c99 -O3 -I$(VENDOR) -I.
LFLAGS_CONV  ?= $(LDFLAGS)

TARGET_BENCH  ?= qoibench
TARGET_LBENCH ?= lqoibench
TARGET_CONV   ?= qoiconv

all: $(TARGET_BENCH) $(TARGET_LBENCH) $(TARGET_CONV)

# Original benchmark (now with a lossy-aware roundtrip verify)
bench: $(TARGET_BENCH)
$(TARGET_BENCH): $(TARGET_BENCH).c qoi.h
	$(CC) $(CFLAGS_BENCH) $(CFLAGS) $(PNG_CFLAGS) $(TARGET_BENCH).c -o $(TARGET_BENCH) $(LFLAGS_BENCH)

# Comprehensive lossy benchmark (PSNR/error metrics, correctness gate, qualitative output).
# Links the renamed lossless-QOI baseline object (-DLQOI_WITH_REF) when present.
lbench: $(TARGET_LBENCH)
$(TARGET_LBENCH): $(TARGET_LBENCH).c qoi.h $(VENDOR)/qoi_ref.o
	$(CC) -std=gnu99 -O3 -DLQOI_WITH_REF -I$(VENDOR) -I. $(CFLAGS) $(PNG_CFLAGS) \
		$(TARGET_LBENCH).c $(VENDOR)/qoi_ref.o -lm -o $(TARGET_LBENCH)

$(VENDOR)/qoi_ref.o: $(VENDOR)/qoi_ref.c $(VENDOR)/qoi_orig.h
	$(CC) -std=c99 -O3 -I$(VENDOR) -c $(VENDOR)/qoi_ref.c -o $(VENDOR)/qoi_ref.o

conv: $(TARGET_CONV)
$(TARGET_CONV): $(TARGET_CONV).c qoi.h
	$(CC) $(CFLAGS_CONV) $(CFLAGS) $(TARGET_CONV).c -o $(TARGET_CONV) $(LFLAGS_CONV)

# Fetch deps + dataset, build, and run the comprehensive benchmark
benchmark:
	./run_benchmark.sh

.PHONY: clean all bench lbench conv benchmark
clean:
	$(RM) $(TARGET_BENCH) $(TARGET_LBENCH) $(TARGET_CONV) $(VENDOR)/qoi_ref.o
