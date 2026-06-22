# CLBG benchmark suite for the MMTk OCaml fork.
#
# Builds the eight Computer Language Benchmarks Game programs with the in-tree
# compiler (the one whose runtime is managed by MMTk). Two outputs per program:
#   build/<b>.byte    bytecode  (runs under any MMTk plan; used by CI validate)
#   build/<b>.native  native    (Immix-family plans only; used by the perf matrix)
#
# Override the toolchain for CI (where only the bytecode compiler is built):
#   make bytecode OCAMLC=$(ROOT)/ocamlc
#
# See run.sh for running/validating, and README.md for the why.

ROOT     := $(abspath ../..)
OCAMLC   ?= $(ROOT)/ocamlc.opt
OCAMLOPT ?= $(ROOT)/ocamlopt.opt
STDLIB   ?= $(ROOT)/stdlib

BENCHES  := fasta nbody spectralnorm binarytrees mandelbrot fannkuchredux knucleotide revcomp
BYTECODE := $(addprefix build/,$(addsuffix .byte,$(BENCHES)))
NATIVE   := $(addprefix build/,$(addsuffix .native,$(BENCHES)))

# .cmi is shared between the bytecode and native builds; serialise to avoid a
# race on it (the programs are tiny, so this costs nothing).
.NOTPARALLEL:

.PHONY: all bytecode native validate matrix golden clean
all: bytecode native
bytecode: $(BYTECODE)
native: $(NATIVE)

build:
	mkdir -p build

build/%.byte: src/%.ml | build
	cd build && OCAMLLIB=$(STDLIB) $(OCAMLC) -I $(STDLIB) -o $*.byte ../src/$*.ml

build/%.native: src/%.ml | build
	cd build && OCAMLLIB=$(STDLIB) $(OCAMLOPT) -I $(STDLIB) -o $*.native ../src/$*.ml

# Correctness across plans (bytecode). Exits nonzero on any mismatch/crash.
validate: bytecode
	./run.sh validate

# Performance matrix (native, Immix-family). Emits results/matrix.csv.
matrix: native
	./run.sh matrix

# Regenerate golden outputs (bytecode, Immix).
golden: bytecode
	./run.sh golden

clean:
	rm -rf build results
