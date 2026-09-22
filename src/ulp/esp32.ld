
MEMORY
{
  ram : org = 0x50000000, len = 0x1000
}

SECTIONS
{
  .text :
  {
    *(.text)
  } > ram
}
