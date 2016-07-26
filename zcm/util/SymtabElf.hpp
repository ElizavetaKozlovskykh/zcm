#pragma once

#include <cstdio>
#include <cstdint>
#include <cassert>
#include <string>

// RRR: should we switch uses of NULL to nullptr?
struct SymtabElf
{
    SymtabElf(const std::string& libname);
    ~SymtabElf();

    bool good() { return f != NULL; }
    bool getNext(std::string& s);

  private:
    bool getNextChar(char& c);
    bool refillBuffer();

  private:
    FILE* f = NULL;
    static const size_t MaxBuf = 4096;
    char buffer[MaxBuf];
    size_t numread = 0;
    size_t index = 0;
};
