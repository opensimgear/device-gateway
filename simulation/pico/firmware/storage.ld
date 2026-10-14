/* The final 72 KiB contains independently erasable gateway/device journals. */
ASSERT(__flash_binary_end <= ORIGIN(FLASH) + LENGTH(FLASH) - 73728,
       "Pico firmware overlaps persistent debug journals")
