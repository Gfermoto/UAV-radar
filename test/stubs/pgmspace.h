#pragma once

#define PROGMEM
#define PGM_P const char *
#define PSTR(s) (s)
#define F(string_literal) (string_literal)

#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#define pgm_read_ptr(addr) (*(const void *const *)(addr))

class __FlashStringHelper {};
