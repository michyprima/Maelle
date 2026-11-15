# Shaders

The `.psh` and `.vsh` files are Latte (R700) assembly, built into `.gsh` files by `latte-assembler`, which only runs
on x86-64 Linux, for example in devkitPro's Docker image:

    docker run --rm --platform linux/amd64 -v "$PWD":/s -w /s devkitpro/devkitppc \
        ./latte-assembler compile gpu3d_aa.gsh --vsh gpu3d.vsh --psh gpu3d_aa.psh

Rebuild the `.gsh` after changing its `.psh`, since the build embeds the `.gsh` as it is. The assembler checks each
clause's `CNT`, the number of 64-bit slots including literals, and says what it expected.

## Packing ALU instructions

The GPU runs an ALU group of up to five instructions, x, y, z and w plus the transcendental slot t, in the time of one,
so the GPU 3D shaders pack independent instructions together. The assembler doesn't check any of the GPU's rules for
groups, and breaking one gives wrong results without an error, so a group follows these:

- A vector instruction goes in the slot of its destination channel, so two can't write the same channel.
- Every instruction in a group reads its sources before any of them writes, so a result can only be read from the
  next group on, and an instruction can write a register that another one in the group still reads.
- Registers are read over three cycles, source 1 of every instruction in the first, source 2 in the second, source 3
  in the third, and each cycle reads one register per channel. So two instructions can't take the same source position
  from the same channel of different registers, like `R4.y` and `R5.y` both as source 1. A source 2 that's the same
  register and channel as its source 1 shares that read.
- Constants (`C0` and up) take at most two reads per group, each covering the x and y or z and w channels of one
  constant.
- The assembler stores a literal word for every literal operand, at the channel the operand names, while the GPU takes
  as many words as the highest channel used needs. So each literal operand gets a channel of its own, `.x` to `.w`,
  even when two operands have the same value, which limits a group to four. Literals equal to 0, 0.5 and 1 become
  inline constants and don't count, while other plain values like `0.25f` take the next free channel.
- Transcendental instructions (`RECIP_IEEE`, `LOG_IEEE`, `EXP_IEEE`) and kills get a group of their own.

These are the rules Mesa's r600 driver follows with every bank swizzle left as the assembler sets it. Packed shaders
gave the same frames as their unpacked versions on the console in Spirit Tracks, Pokemon Black and Pokemon Black 2,
which `./build-wiiu-debug.sh gpucrc` checks.
