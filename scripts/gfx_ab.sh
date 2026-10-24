#!/bin/bash
# gfx_ab.sh MODE -- renderer experiments that need one binary (scripts/README: same-binary A/B).
#
#   gen  a build whose renderer counts its branches (-fprofile-generate); every
#        GCOV_TASKS tasks it writes them to host0:/pgo (work/psplink/pgo). Run
#        replays / the game with it to collect a profile.
#   aa   an A/B build whose two renderers are compiled alike: what moving the
#        code alone does (the control).
#   pgo  an A/B build whose renderer B is built from that profile
#        (-fprofile-use: hot/cold splitting, block order, inlining by counts).
#   src  an A/B build whose renderer A is compiled from the sources in A_SRC
#        (a copy of runtime/src/gfx from before a change) and B from today's:
#        a source change measured in one binary.
#
# An A/B build carries the renderer twice, each copy's symbols prefixed A_ / B_,
# and call stubs for the ten rt_gfx_* functions the rest of the runtime uses:
# gfx_b.txt next to the PRX runs B (spram.c reads it; B's scratchpad block is
# installed instead of A's). Uses build/psp's other objects: run gmake first.
# Output: build/ab/<MODE>/afpsp.prx (+ .elf).
set -e
MODE=${1:?usage: gfx_ab.sh gen|aa|pgo|src}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PSPDEV=${PSPDEV:-$HOME/pspdev}
export PATH=$PSPDEV/bin:$PATH
PSPSDK=$PSPDEV/psp/sdk
BASE=build/psp
OUT=build/ab/$MODE
PGO_OBJ=build/ab/pgo_obj      # gen and pgo compile the renderer to the same paths (the profile is named by them)
PGO_DIR=work/psplink/pgo
mkdir -p "$OUT" "$PGO_OBJ" "$PGO_DIR"

CFLAGS="-G0 -mno-check-zero-division -fno-math-errno -fno-strict-aliasing -ffunction-sections -fdata-sections
 -O2 -g -w -Iruntime/include -Iruntime/src -Iwork/recomp_out -I$PSPDEV/psp/include -I$PSPSDK/include -D_PSP_FW_VERSION=600"
PROFILE_PATH="-fprofile-prefix-path=$ROOT/$PGO_OBJ"

compile_gfx() { # dir extra-flags...  (sources from $SRC, default runtime/src/gfx)
    local dir=$1; shift
    mkdir -p "$dir"
    for src in ${SRC:-runtime/src/gfx}/*.c; do
        psp-gcc $CFLAGS "$@" -c "$src" -o "$dir/$(basename "${src%.c}").o"
    done
}

# one relocatable object of a renderer copy, every global it defines prefixed
pack_copy() { # dir prefix out.o [rename spram sections]
    local dir=$1 prefix=$2 out=$3
    # the PRX linker script takes .text.* in input order: keep the profile's hot
    # functions together, and the cold parts out of their way
    cat > "$out.ld" <<'LD'
SECTIONS {
  .text.hot_group : { *(.text.hot .text.hot.*) }
  .text.unlikely_group : { *(.text.unlikely .text.unlikely.*) }
}
LD
    psp-ld -r -T "$out.ld" -o "$out.tmp" "$dir"/*.o
    psp-nm -g --defined-only "$out.tmp" | awk -v p="$prefix" 'NF == 3 { print $3, p $3 }' > "$out.syms"
    local extra=()
    if [ "${4:-}" = rename ]; then
        extra=(--rename-section spram_text=spram_text_b --rename-section spram_ptrs=spram_ptrs_b)
    fi
    psp-objcopy --redefine-syms="$out.syms" "${extra[@]}" "$out.tmp" "$out"
    rm -f "$out.tmp"
}

# the runtime's objects for today's sources (build/psp/rt may hold some of deleted ones)
OTHER_RT=$(for f in runtime/src/*.[cS] runtime/src/audio/*.[cS] runtime/src/english/*.[cS]; do
    o=${f#runtime/src/}; echo "$BASE/rt/${o%.*}.o"; done)
GEN_OBJS=$(ls $BASE/gen/*.o)
LIBS="-lme-core -lpspsdk -lpspgu -lpsppower -lpsprtc -lpspaudio -lpspdisplay -lpspge -lpspctrl -lm"
LDFLAGS="-L$PSPDEV/psp/lib -L$PSPSDK/lib -specs=$PSPSDK/lib/prxspecs -Wl,-q,-T$PSPSDK/lib/linkfile.prx
 -Wl,-zmax-page-size=128 -Wl,--gc-sections -Wl,--wrap=_sbrk"

link() { # objects...
    local id
    id=$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n')
    echo "const unsigned int rt_build_id = 0x$id;" > "$OUT/build_id.c"
    psp-gcc $CFLAGS -c "$OUT/build_id.c" -o "$OUT/build_id.o"
    psp-gcc $LDFLAGS $PSPSDK/lib/prxexports.o $OTHER_RT "$OUT/build_id.o" "$@" $GEN_OBJS $BASE/me/me_synth_gen.o \
        $LIBS -o "$OUT/afpsp.elf"
    psp-fixup-imports "$OUT/afpsp.elf"
    psp-prxgen "$OUT/afpsp.elf" "$OUT/afpsp.prx"
    echo "built $OUT/afpsp.prx"
}

thunks() {
    cat > "$OUT/gfx_ab_thunks.S" <<'EOF'
	.set noreorder
	.text
	.macro thunk name
	.globl \name
	.ent \name
\name:
	lui	$t0, %hi(rt_gfx_variant)
	lw	$t0, %lo(rt_gfx_variant)($t0)
	bnez	$t0, 1f
	nop
	j	A_\name
	nop
1:	j	B_\name
	nop
	.end \name
	.endm
	thunk rt_gfx_capture
	thunk rt_gfx_capture_frame
	thunk rt_gfx_idle
	thunk rt_gfx_init
	thunk rt_gfx_present
	thunk rt_gfx_replay
	thunk rt_gfx_submit_task
	thunk rt_gfx_toggle_stretch
	thunk rt_gfx_yield
	thunk rt_gfx_yielded
EOF
    psp-gcc -G0 -c "$OUT/gfx_ab_thunks.S" -o "$OUT/gfx_ab_thunks.o"
}

case $MODE in
gen)
    rm -f "$PGO_OBJ"/*.gcda
    compile_gfx "$PGO_OBJ" -fprofile-generate=host0:/pgo $PROFILE_PATH -fprofile-update=single -DRT_PGO_GEN
    link "$PGO_OBJ"/*.o -lgcov
    ;;
src)
    SRC=${A_SRC:?A_SRC=directory with the earlier runtime/src/gfx} compile_gfx "$OUT/A"
    pack_copy "$OUT/A" A_ "$OUT/gfx_A.o"
    compile_gfx "$OUT/B"
    pack_copy "$OUT/B" B_ "$OUT/gfx_B.o" rename
    thunks
    link "$OUT/gfx_A.o" "$OUT/gfx_B.o" "$OUT/gfx_ab_thunks.o"
    ;;
aa|pgo)
    compile_gfx "$OUT/A"
    pack_copy "$OUT/A" A_ "$OUT/gfx_A.o"
    if [ "$MODE" = aa ]; then
        compile_gfx "$OUT/B"
        pack_copy "$OUT/B" B_ "$OUT/gfx_B.o" rename
    else
        # The profile's block order, function order and hot/cold splitting, without the
        # code-growing passes -fprofile-use also turns on (unrolling, peeling, tracing,
        # cloning): fewer instruction cache lines is the point. PGO_FLAGS adds more.
        compile_gfx "$PGO_OBJ" -fprofile-use="$ROOT/$PGO_DIR" $PROFILE_PATH -fprofile-partial-training \
            -freorder-blocks-and-partition -fno-unroll-loops -fno-peel-loops -fno-tracer -fno-ipa-cp-clone \
            -fno-unswitch-loops -fno-split-loops -fno-loop-interchange -fno-loop-unroll-and-jam \
            -fno-tree-loop-distribution -fno-predictive-commoning -fno-unroll-completely-grow-size ${PGO_FLAGS:-}
        pack_copy "$PGO_OBJ" B_ "$OUT/gfx_B.o" rename
    fi
    thunks
    link "$OUT/gfx_A.o" "$OUT/gfx_B.o" "$OUT/gfx_ab_thunks.o"
    ;;
*)
    echo "usage: gfx_ab.sh gen|aa|pgo|src"; exit 1 ;;
esac
