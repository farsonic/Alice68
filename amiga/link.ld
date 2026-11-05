/* one hunk: code, constants, data and zeroed bss together, based at 0;
   --emit-relocs keeps the absolute relocations for elf2hunk.py */
ENTRY(_start)
SECTIONS
{
    . = 0;
    .text : {
        *(.text.start)
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
