// Fake EEPROM: a byte array, erased state 0xFF, with a commit counter. Mirrors the ESP32 Arduino EEPROM API subset used.
#pragma once
#include <stdint.h>
#include <string.h>

struct FakeEEPROM {
    static const int SIZE = 1024;
    uint8_t data[SIZE];
    int commits = 0;
    FakeEEPROM() { erase(); }
    void erase() { memset(data, 0xFF, SIZE); commits = 0; }
    uint8_t read(int addr) const { return (addr >= 0 && addr < SIZE) ? data[addr] : 0xFF; }
    void write(int addr, uint8_t value) { if (addr >= 0 && addr < SIZE) data[addr] = value; }
    bool commit() { commits++; return true; }
};
extern FakeEEPROM EEPROM;
