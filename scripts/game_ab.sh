#!/bin/bash
# game_ab.sh MODE -- game-code experiments that need one binary (see runtime/src/game_ab.c).
#
#   aa   an A/B build whose second game copy is the same objects again: what
#        moving the code alone does (the control).
#   gen  a build whose game code counts its branches (-fprofile-generate); the
#        renderer worker writes the counts to host0:/pgo_game every PGO_TASKS
#        tasks (default 1800). Run the game with it to collect a profile.
#   pgo  an A/B build whose second copy is compiled from that profile: block
#        order (cold blocks to the ends of functions) and function order by
#        counts, without the code-growing passes, its hot functions grouped
#        together. (Not -freorder-blocks-and-partition: the split-off halves are
#        reached by 16-bit branches, which can't span the grouped hot code.)
#
# game_b.txt next to the PRX runs the second copy. Uses build/psp's runtime and
# game objects: run gmake first. Output: build/ab/game_<MODE>/afpsp.prx (+ .elf).
set -e
MODE=${1:?usage: game_ab.sh aa|gen|pgo}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PSPDEV=${PSPDEV:-$HOME/pspdev}
export PATH=$PSPDEV/bin:$PATH
PSPSDK=$PSPDEV/psp/sdk
BASE=build/psp
GEN=work/recomp_out
OUT=build/ab/game_$MODE
PGO_OBJ=build/ab/game_pgo_obj # gen and pgo compile to the same paths (the profile is named by them)
PGO_DIR=work/psplink/pgo_game
JOBS=${JOBS:-10}
mkdir -p "$OUT" "$PGO_OBJ" "$PGO_DIR"

INC="-Iruntime/include -Iruntime/src -I$GEN -I$PSPDEV/psp/include -I$PSPSDK/include -D_PSP_FW_VERSION=600"
BASEFLAGS="-G0 -mno-check-zero-division -fno-math-errno -fno-strict-aliasing -ffunction-sections -fdata-sections"
GEN_CFLAGS="$BASEFLAGS -O2 -w $INC -DRECOMP_GENERATED_CODE"
RT_CFLAGS="$BASEFLAGS -O2 -g -w $INC"
PROFILE_PATH="-fprofile-prefix-path=$ROOT/$PGO_OBJ"
GROWTH_OFF="-fno-unroll-loops -fno-peel-loops -fno-tracer -fno-ipa-cp-clone -fno-unswitch-loops -fno-split-loops
 -fno-loop-interchange -fno-loop-unroll-and-jam -fno-tree-loop-distribution -fno-predictive-commoning
 -fno-unroll-completely-grow-size"

compile_gen() { # dir extra-flags...
    local dir=$1; shift
    mkdir -p "$dir"
    local flags="$GEN_CFLAGS $*"
    export flags dir
    ls $GEN/funcs_*.c | xargs -P "$JOBS" -I{} sh -c 'f={}; n=$(basename "${f%.c}"); psp-gcc $flags -c "$f" -o "$dir/$n.o"'
}

RT_OBJS=$(for f in runtime/src/*.[cS] runtime/src/audio/*.[cS] runtime/src/english/*.[cS] runtime/src/gfx/*.[cS]; do
    o=${f#runtime/src/}; echo "$BASE/rt/${o%.*}.o"; done)
LIBS="-lme-core -lpspsdk -lpspgu -lpsppower -lpsprtc -lpspaudio -lpspdisplay -lpspge -lpspctrl -lm"
LDFLAGS="-L$PSPDEV/psp/lib -L$PSPSDK/lib -specs=$PSPSDK/lib/prxspecs -Wl,-q,-T$PSPSDK/lib/linkfile.prx
 -Wl,-zmax-page-size=128 -Wl,--gc-sections -Wl,--wrap=_sbrk"

link() { # runtime-objects game-objects...
    local rt=$1; shift
    local id
    id=$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n')
    echo "const unsigned int rt_build_id = 0x$id;" > "$OUT/build_id.c"
    psp-gcc $RT_CFLAGS -c "$OUT/build_id.c" -o "$OUT/build_id.o"
    psp-gcc $LDFLAGS $PSPSDK/lib/prxexports.o $rt "$OUT/build_id.o" "$@" $BASE/me/me_synth_gen.o $LIBS -o "$OUT/afpsp.elf"
    psp-fixup-imports "$OUT/afpsp.elf"
    psp-prxgen "$OUT/afpsp.elf" "$OUT/afpsp.prx"
    echo "$id" | tr a-f A-F > "$OUT/build_id.txt"
    echo "built $OUT/afpsp.prx"
}

# the second copy: one relocatable object, every global prefixed B_ (hot code grouped first),
# and the table of both addresses of every function for game_ab.c
pack_b() { # objects...
    cat > "$OUT/b.ld" <<'LD'
SECTIONS {
  .text.hot_group : { *(.text.hot .text.hot.*) }
  .text.unlikely_group : { *(.text.unlikely .text.unlikely.*) }
}
LD
    psp-ld -r -T "$OUT/b.ld" -o "$OUT/game_B.tmp" "$@"
    psp-nm -g --defined-only "$OUT/game_B.tmp" | awk 'NF == 3 { print $3, "B_" $3 }' > "$OUT/game_B.syms"
    psp-objcopy --redefine-syms="$OUT/game_B.syms" "$OUT/game_B.tmp" "$OUT/game_B.o"
    rm -f "$OUT/game_B.tmp"
    {
        echo '	.section .rodata.game_ab, "a"'
        echo '	.align 2'
        echo '	.globl rt_game_ab_pairs'
        echo 'rt_game_ab_pairs:'
        psp-nm -g --defined-only "$OUT/game_B.o" | awk 'NF == 3 && ($2 == "T" || $2 == "t") { a = substr($3, 3); print "\t.word " a ", " $3 }'
        echo '	.globl rt_game_ab_count'
        echo 'rt_game_ab_count:'
        echo "	.word $(psp-nm -g --defined-only "$OUT/game_B.o" | awk 'NF == 3 && ($2 == "T" || $2 == "t")' | wc -l)"
    } > "$OUT/game_ab_pairs.S"
    psp-gcc -G0 -c "$OUT/game_ab_pairs.S" -o "$OUT/game_ab_pairs.o"
}

case $MODE in
aa)
    pack_b $BASE/gen/*.o
    link "$RT_OBJS" $BASE/gen/*.o "$OUT/game_B.o" "$OUT/game_ab_pairs.o"
    ;;
gen)
    compile_gen "$PGO_OBJ" -fprofile-generate=host0:/pgo_game $PROFILE_PATH -fprofile-update=single
    # the renderer worker writes the counts out (gfx_worker.c, RT_PGO_GEN)
    mkdir -p "$OUT/rt"
    psp-gcc $RT_CFLAGS -DRT_PGO_GEN -DRT_PGO_TASKS=${PGO_TASKS:-1800} -c runtime/src/gfx/gfx_worker.c -o "$OUT/rt/gfx_worker.o"
    psp-gcc $RT_CFLAGS -DRT_PGO_GEN -c runtime/src/gfx/gfx_debug.c -o "$OUT/rt/gfx_debug.o"
    rt=$(echo "$RT_OBJS" | grep -v '/gfx/gfx_worker.o$' | grep -v '/gfx/gfx_debug.o$')
    link "$rt $OUT/rt/gfx_worker.o $OUT/rt/gfx_debug.o" "$PGO_OBJ"/*.o -lgcov
    ;;
pgo)
    compile_gen "$PGO_OBJ" -fprofile-use="$ROOT/$PGO_DIR" $PROFILE_PATH -fprofile-partial-training -fprofile-correction \
        $GROWTH_OFF ${PGO_FLAGS:-}
    pack_b "$PGO_OBJ"/*.o
    link "$RT_OBJS" $BASE/gen/*.o "$OUT/game_B.o" "$OUT/game_ab_pairs.o"
    ;;
*)
    echo "usage: game_ab.sh aa|gen|pgo"; exit 1 ;;
esac
