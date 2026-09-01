#pragma once

#include <stdint.h>
#include <stddef.h>

class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t) { return 0; }
    virtual size_t write(const uint8_t *buffer, size_t size) {
        size_t n = 0;
        while (size--) n += write(*buffer++);
        return n;
    }
    size_t print(const char *) { return 0; }
};

class Stream : public Print {
public:
    virtual int available() { return 0; }
    virtual int read() { return -1; }
    virtual int peek() { return -1; }
    size_t readBytes(char *, size_t length) { return length; }
};

class Printable {
public:
    virtual ~Printable() = default;
    virtual size_t printTo(Print &print) const = 0;
};
