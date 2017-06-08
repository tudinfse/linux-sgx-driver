
# Inline Asm for ENCLS Privileged Instruction

All kernel-mode SGX system functions are executed via a single x86 `ENCLS` instruction (hex `0F 01 CF`).
`ENCLS` receives a system function in `rax` register and additional arguments in `rbx`, `rcx`, and `rdx` registers.
For example, `ENCLS(EADD)` issues an `ENCLS` instruction with `rax=01`.

The driver uses inline assembly to issue `ENCLS` as below:

* `__encls_ret()` -- instruction that returns info/error code in `rax`

```
1: .byte 0x0f, 0x01, 0xcf;
2:
.section .fixup,"ax"
3: jmp 2b
.previous
_ASM_EXTABLE(1b, 3b)
```

* `__encls()` -- instruction that returns nothing

```
1: .byte 0x0f, 0x01, 0xcf;
   xor %eax, %eax;
2:
.section .fixup,"ax"
3: movq $-1, %rax
   jmp 2b
.previous
_ASM_EXTABLE(1b, 3b)
```

## Explanations

This document covers all not immediately obvious constructs: https://www.kernel.org/doc/Documentation/x86/exception-tables.txt

* `b` in `jmp 2b` stands for "jump backwards" (for "jump forwards" there is `jmp 2f`)

* `.section .fixup,"ax"` puts the following code (`movq` and `jmp 2b` at label 3) into special section "fixup"
  - this code is executed whenever a fault happens on `ENCLS` instruction
  - for the `ret` case, the error code is already in `rax` and no need to do anything special (only jump after `ENCLS`)
  - for the second case, set error code `rax=-1` and then jump after `ENCLS`

* `.previous` allows to jump to the previously used section (which is `.text` in this particular assembly)
  - see https://stackoverflow.com/questions/2416879/what-does-asm-previous-mean
  - seems useless in this scenario, see examples without it: https://www.kernel.org/doc/Documentation/x86/exception-tables.txt
  - also note that `_ASM_EXTABLE()` already jumps to the previously used section: http://elixir.free-electrons.com/linux/v3.18/source/arch/x86/include/asm/asm.h#L47

* `_ASM_EXTABLE(1b, 3b)` creates a pair (instruction-that-might-fault, fault-handler) in section "__ex_table"
  - `1b` is the first section (`ENCLS` instruction), `2b` is the second section (empty, after `ENCLS`)
