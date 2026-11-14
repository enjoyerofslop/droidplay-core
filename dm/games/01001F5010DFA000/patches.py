# Pokémon Legends: Arceus (01001F5010DFA000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py; MATCH_CUTS as described there
# ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# 60 fps (30 on the Switch) on 1.1.1 (v262144) main, with game time at real time. Two things hold the game at 30, and the stubs move both to the host's frame step:
# - The present interval. The start-up code builds the display's settings on the stack (main+0x277d7xx) and hands them to the display setup (main+0x29d6a64 → main+0x2aa24c4),
#   which sets the window's present interval to that word + 1 (main+0x2aa2788). The word is 1: interval 2. The first stub makes it 0 (interval 1) when the state page's target is
#   not 0, read once at start-up (the loader writes the target before the game runs); at target 0 it stays 1, the Switch's 30
# - The frame period. The game steps its time by a stored period, not by the clock: the frame-time object's constructor (main+0x277bda4) stores 33,333,334 ns in four {0, ns} pairs
#   (object +0x78 to +0xb0), and its interface at +0x70 (vtable main+0x4171538) reads and sets them: [0] main+0x278303c returns the address of pair idx, [1] main+0x2783048 sets
#   a pair from the table {16,666,667, 33,333,334, 50,000,000, 66,666,667} at main+0x397e8d0, [2] main+0x277bef4 sets a pair's ns. With interval 1 alone a frame shown every vsync still stepped 1/30 s and
#   the game ran at double speed (Odin 3, Adreno 830, 2026-11-10; with the period at 16,666,667 ns, 60 fps ran at normal speed and 30 fps at half). The stub in the getter stores
#   the host's step (the vsyncs the frame stays on screen, state page +0x20) times 16,666,667 ns in the pair asked for, before the caller reads it: 60 fps steps 1/60 s, 30 fps
#   (target 30, heat, a movie, late frames) 1/30 s. The getter is in this vtable only (no other reference in main). Before the host has decided a step, and at target 0, the
#   stored period stays
# The third stub counts the vsyncs stepped where the scene update reads the period (main+0x279a6f4), for the host's game time log. x16 and x17: unused from main+0x277d838 to the
# bl at main+0x277d84c; dead in a function entered through blr (the getter); x8 at main+0x279a6f4 is set by the next instruction
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fs_site(0x277d838, '280080d2')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (float bits; 0 = off)
              0xd2800028,   # mov x8,#1              (original) interval 2
              0x34000051,   # cbz w17,back (+2)
              0xd2800008):  # mov x8,#0              interval 1
        emit(w)
    emit(B(here(), 0x277d83c))                                       # back: movk x8,#2,lsl #48
    fs_put(0x277d838, [B(0x277d838, start)], 'frame step: display settings, present interval 1 with a target fps (state page; 0 = the game\'s interval 2)')
    fs_site(0x278303c, '08d0218b00210091c0035fd6')
    start = here(); emit(0x8b21d008)                                 # add x8,x0,w1,sxtw #4   (original)
    emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps
              0x340000f1,   # cbz w17,back (+7)
              0xb9402211,   # ldr w17,[x16,#0x20]    step in vsyncs
              0x340000b1,   # cbz w17,back (+5)      not decided yet
              0x528a0570,   # mov w16,#0x502b
              0x72a01fd0,   # movk w16,#0xfe,lsl #16 16,666,667 ns
              0x9bb07e31,   # umull x17,w17,w16
              0xf9000911):  # str x17,[x8,#0x10]     the pair's ns
        emit(w)
    emit(B(here(), 0x2783040))                                       # back: add x0,x8,#8; ret
    fs_put(0x278303c, [B(0x278303c, start)], 'frame step: the frame period read is the host\'s vsyncs × 16,666,667 ns (state page target fps; 0 = the stored period)')
    fs_site(0x279a6f4, '000440fd')
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]
              0x340000d1,   # cbz w17,orig (+6)
              0xb9402211,   # ldr w17,[x16,#0x20]
              0x34000091,   # cbz w17,orig (+4)
              0xf9401a08,   # ldr x8,[x16,#0x30]
              0x8b110108,   # add x8,x8,x17
              0xf9001a08):  # str x8,[x16,#0x30]     vsyncs stepped
        emit(w)
    emit(0xfd400400); emit(B(here(), 0x279a6f8))                     # orig: ldr d0,[x0,#8]; back
    fs_put(0x279a6f4, [B(0x279a6f4, start)], 'frame step: counts the vsyncs the scene update steps')

FRAME_STEP = {'aee8f150dda1b5a838806e1a5ea6827a': frame_step}

# A workaround in the game's own code; the cause is open (the note's "Crash in the capture demo", GitHub issue #712). main+0x16e33ec reads a weak reference at this+0x88, locks it and
# then reads [result + 0x118] without checking the result. The capture demo task calls it once per ball (main+0x19d2368 from main+0x1ccf1a4). Null there was a data abort at
# main+0x16e34fc after 37 to 65 minutes of play. The reference is null from the object's constructor until the field setup's state 0x18 (main+0x1877a4c through main+0x16dde90), and the
# lock returns null for a dead object; which of the two the crashes are is unknown. The cuts return the function's empty result ({0, 0} through x20) on both, before it takes any reference.
# All 12 direct reads of the result (the ball's +0x810) null-check it.
#   main+0x16e33ec, 2672 bytes, in 1.1.1. The room is two of the function's five identical blocks (mov w23, wzr; b main+0x16e3a14) at main+0x16e3da4, whose two branches move to the
#   block at main+0x16e3d88. Taking the function's abort stub instead, as the issue's first form did, moves its callers to a stub in the next function, which a function match cannot hold.
capture = 'capture demo: the empty result when the weak reference at +0x88 is unset or dead'

# A data race in the game's own code (the note's "Title screen freeze"). The render job system's fork/join (1.1.1 main+0x2ac2460) hands two helper jobs to the other cores' render
# workers and counts each in a shared word (+0x138 of the join) with a plain ldr / add / str, outside the join's mutex (+0x118). Each helper subtracts one under that mutex when
# its share is done (main+0x2ac2f64, main+0x2ac3038), and the leader waits under it until the count is back to 1. A helper that subtracts between the leader's load and store
# of its second count loses one of the two updates: the count ends at 0 or 2, and the leader, the dispatcher waiting for it and the main thread wait for good. The picture stops,
# sound goes on (the idle title on a OnePlus Pad 3 at 60 fps). Each of the seven updates becomes one atomic add of +1 or -1 to the same word (stadd w9, [x8]; LSE,
# which every host CPU has: Android hosts are built for armv8.2-a+lse, Apple's from M1 and A13). x8 and x9 are free there: each update is followed by a call through a PLT stub.
#   main+0x2ac2460 (2576 bytes: both forks' counts and the leader's last subtraction), main+0x2ac2f64 (184) and main+0x2ac3038 (208, the helpers), in 1.1.1
join = 'fork/join count: each update one atomic add'
join_count = {'name': join, 'functions': [
    {'size': 2576, 'fingerprints': ['4bfde3d8cc52150dfb1f1213d6f68fa71afa200aba4463a8d0dea1f66e22f1ce'],
     'cuts': [
         # ldr w8, [x27, #0x138]; …; add w8, w8, #1; mov x0, xN; str w8, [x27, #0x138]  →  add x8, x27, #0x138; …; mov w9, #1; mov x0, xN; stadd w9, [x8]
         (124, '683b41b95423119108050011e00314aa683b01b9', '68e304915423119129008052e00314aa1f0129b8', join + ': the first fork, first helper (as 1.1.1 main+0x2ac24dc)'),
         (256, '683b41b908050011e00314aa683b01b9', '68e3049129008052e00314aa1f0129b8', join + ': the first fork, second helper (as 1.1.1 main+0x2ac2560)'),
         (1724, '683b41b99523119108050011e00315aa683b01b9', '68e304919523119129008052e00315aa1f0129b8', join + ': the second fork, first helper (as 1.1.1 main+0x2ac2b1c)'),
         (1856, '683b41b908050011e00315aa683b01b9', '68e3049129008052e00315aa1f0129b8', join + ': the second fork, second helper (as 1.1.1 main+0x2ac2ba0)'),
         # the leader's own last subtraction, under the mutex: sub w8, w8, #1  →  mov w9, #-1
         (2360, '683b41b908050051e00314aa683b01b9', '68e3049109008012e00314aa1f0129b8', join + ': the leader done (as 1.1.1 main+0x2ac2d98)'),
     ]},
    {'size': 184, 'fingerprints': ['c1ece3129d93d80ee325a36a72516d6ea32fa40503b15463f23cfa5f25503365'],
     'cuts': [(140, '883a41b90805005180220491883a01b9', '88e2049109008012802204911f0129b8', join + ': a helper done (as 1.1.1 main+0x2ac2ff0)')]},
    {'size': 208, 'fingerprints': ['14b35b021f36ae8a552c0d5008379f31be489592d82f5ad6de7393cdc001976b'],
     'cuts': [(164, '883a41b90805005180220491883a01b9', '88e2049109008012802204911f0129b8', join + ': a helper done (as 1.1.1 main+0x2ac30dc)')]},
]}
MATCH_CUTS = {'main': [
    {'name': capture, 'size': 2672, 'fingerprints': ['123f93c4fa74d68f6100dab6ec8310a5047b2b3535b72361b6e658e3e48d792e'],
     'cuts': [
         # stur xzr, [x29, #-0x60]  →  b the empty result
         (48, 'bf031af8', '64020014', capture + ': no reference at +0x88 (as 1.1.1 main+0x16e341c)'),
         # b main+0x16e3420  →  b the null check
         (1084, 'fefeff17', '5f010014', capture + ': after the lock (as 1.1.1 main+0x16e3828)'),
         # b.ne to the freed blocks  →  b.ne to the identical block at main+0x16e3d88
         (1480, '811f0054', 'a11e0054', capture + ': frees a duplicate block (as 1.1.1 main+0x16e39b4)'),
         (1552, '811d0054', '611c0054', capture + ': frees a duplicate block (as 1.1.1 main+0x16e39fc)'),
         # the null check: ldur x8, [x29, #-0x60]; cbnz x8, main+0x16e3420. The empty result: stp xzr, xzr, [x20]; b the epilogue at main+0x16e3cd0
         (2488, 'f7031f2a1bffff17f7031f2a19ffff17', 'a8035af8c8b3ffb59f7e00a9c8ffff17', capture + ': the null check and the empty result (as 1.1.1 main+0x16e3da4)'),
     ]},
    join_count,
]}

# How general each patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'capture demo: the empty result when the weak reference at +0x88 is unset or dead', 'now': 'game', 'goal': 'game', 'purpose': 'runtime',
     'why': "the game's own function dereferences its own weak reference; no host service is on the path, so no host or engine rule can hold it. Whether another Game Freak game shares the function is unchecked",
     'checked': "function-matched on 1.1.1, the only build on hand: exactly one match, a boot to the field setup, and the patched function run on the module's own bytes in a small interpreter for both null paths. Issue #712's reporters ran the earlier cuts for about 7 hours with no crash; the path itself has not been reached with these cuts"},
    {'what': 'fork/join count: each update one atomic add', 'now': 'game', 'goal': 'game', 'purpose': 'runtime',
     'why': "a data race in the game's own render job system (a count updated outside its mutex on one side); no host service is on the path, and no host rule can make a plain load and store atomic. Whether Game Freak's other games share the functions is unchecked",
     'checked': "function-matched on 1.1.1, the only build on hand: each of the three functions matches once and the rewriter writes the seven cuts. On a OnePlus Pad 3 at the 60 fps default, idle at the title (the note's \"Title screen freeze\"): 0 of 20 starts froze, against 1 of 20 without the cuts; with a ~1 ms spin inside the leader's second count, 0 of 6 against 4 of 4"},
    {'what': 'present interval 2 → 1 with a target fps, and the frame period from the host\'s frame step', 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own display settings word and frame-time object; MATCH_FRAME_STEP could carry the three stubs (the start-up function, the period getter and the scene update), not tried: 1.1.1 is the only build on hand"},
]
