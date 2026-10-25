Mapper test ROMs for the two rom_database entries the v1.10 compiler bug
reached (gcc 6.5.0b tested bit 24 instead of bit 0, see the v1.11 changelog).
They are not game dumps: each is a 64 KB ROM of our own code whose header
checksum ($0000) and word sum from $200 (set by the last word) match the entry,
so md_cart_init() installs that cartridge's handler.

  kanzume_test.md  Game no Kanzume Otokuyou entry -> mapper_seganet_w:
                   write 1 to $A130F1, then a byte into ROM; must be ignored.
  tekken_test.md   Tekken 3 Special entry -> tekken_regs_w / tekken_regs_r:
                   $FF to $400000, $01 to $400004, $400003 must read $00.

Backdrop green = pass, red = fail.

Build (PC):
  V=D:/amiga-gcc-amiga6/bin/vasmm68k_mot.exe
  $V -quiet -Fbin -m68000 -DSEGANET=1 -o seganet.bin maptest.s
  $V -quiet -Fbin -m68000 -DTEKKEN=1  -o tekken.bin  maptest.s
  python -I mkrom.py seganet.bin kanzume_test.md F9D1
  python -I mkrom.py tekken.bin  tekken_test.md  C2F0
(vasm warns that the label "fail" conflicts with a directive; the branch
still resolves to the red store, check with objdump -D -b binary -m m68k:68000.)

Result 2026-10-25, A1200 with a PiStorm (Emu68, CM4), CaffeineOS, run with -nogl:
  v1.10: both red.  v1.11: both green.
