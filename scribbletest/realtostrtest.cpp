// realToStr (ulib/stringutil.h) must write exactly the bytes the original digit-at-a-time version wrote:
//  it is the number formatter of every saved document.  This keeps that original version verbatim and
//  compares the two on tens of millions of values - random bit patterns (NaN, inf, huge, tiny), random
//  ratios, decimal ties like x.xxx5 and their neighbours, values near INT_MAX, -0 - for every prec 0-9,
//  as double and as float.
//
//   g++ -std=c++14 -O2 -I ulib -isystem stb scribbletest/realtostrtest.cpp -o realtostrtest && ./realtostrtest [millions]
//   (one command, run from the repo root; exits with the number of mismatches)

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <limits>
#include "stringutil.h"
#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"

// the original, before the digit pair table
template<typename Real>
int realToStrOld(char* str, Real f, int prec)
{
  static const Real powers_of_10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

  if(!(f < INT_MAX && f > -INT_MAX && prec < 10))
    return stbsp_sprintf(str, "%.*f", prec, f);

  bool negative = f < 0;
  if(negative)
    f = -f;
  int whole = int(f);
  int frac = int((f - whole) * powers_of_10[prec] + Real(0.5));
  int ii = 0;
  if(frac != 0) {
    while(prec > 0 && frac % 10 == 0) { frac /= 10; --prec; }
    while(ii < prec) {
      str[ii++] = '0' + char(frac % 10);
      frac /= 10;
    }
    if(ii > 0)
      str[ii++] = '.';
  }
  else if(whole == 0) {
    str[ii] = '0';
    return 1;
  }
  if(frac >= 1)
    whole += 1;
  do {
    str[ii++] = '0' + char(whole % 10);
    whole /= 10;
  } while(whole > 0);
  if(negative)
    str[ii++] = '-';
  std::reverse(str, str+ii);
  return ii;
}

static uint64_t rngState = 0x9E3779B97F4A7C15ull;
static uint64_t rng64()
{
  // splitmix64
  uint64_t z = (rngState += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static double rngUnit() { return (rng64() >> 11) * (1.0/9007199254740992.0); }

static long nCompared = 0, nMismatch = 0;

template<typename Real>
static void compareOne(Real value, int prec)
{
  char oldBuf[512], newBuf[512];
  memset(oldBuf, 'x', 64);  memset(newBuf, 'y', 64);
  int oldLen = realToStrOld(oldBuf, value, prec);
  int newLen = realToStr(newBuf, value, prec);
  ++nCompared;
  if(oldLen != newLen || memcmp(oldBuf, newBuf, oldLen) != 0) {
    if(++nMismatch <= 20)
      printf("MISMATCH %s %.17g prec %d: old '%.*s' new '%.*s'\n", sizeof(Real) == 8 ? "double" : "float",
          double(value), prec, oldLen, oldBuf, newLen, newBuf);
  }
}

template<typename Real>
static void compareAllPrec(Real value)
{
  for(int prec = 0; prec < 10; ++prec)
    compareOne(value, prec);
}

static double randomValue()
{
  static const double scales[] = {1e-12, 1e-6, 1e-3, 0.1, 1, 10, 1000, 1e5, 1e7, 1e9, 2.2e9, 1e12};
  switch(rng64() % 8) {
  case 0: { uint64_t bits = rng64(); double value; memcpy(&value, &bits, 8); return value; }  // any bit pattern
  case 1: return (rng64() & 1 ? 1 : -1) * double(rng64() % 100000)/double(rng64() % 1000 + 1);
  case 2: return (rng64() & 1 ? 1 : -1) * rngUnit() * scales[rng64() % 12];
  case 3: {  // decimal ties d.ddd5 at some precision, and their float neighbours
    int prec = int(rng64() % 10);
    double tie = (double(int64_t(rng64() % 2000000000) - 1000000000) + 0.5) / pow(10.0, prec);
    int step = int(rng64() % 5) - 2;
    for(; step > 0; --step) tie = nextafter(tie, INFINITY);
    for(; step < 0; ++step) tie = nextafter(tie, -INFINITY);
    return tie;
  }
  case 4: {  // near +/- INT_MAX
    double value = INT_MAX - double(rng64() % 4096) * 0.25 + rngUnit()*4;
    return rng64() & 1 ? -value : value;
  }
  case 5: {  // just below integers: 1.9999999999
    double value = double(int64_t(rng64() % 200000) - 100000) - rngUnit()*1e-9;
    return value;
  }
  case 6: return (rng64() & 1 ? 1 : -1) * rngUnit() * 1e-9;  // rounds to 0 / -0 at most precisions
  default: return (rng64() & 1 ? 1 : -1) * double(rng64() % 100000000) / 1000.0;  // typical 3 digit coordinates
  }
}

int main(int argc, char* argv[])
{
  long millions = argc > 1 ? atol(argv[1]) : 40;
  const double specials[] = {0.0, -0.0, NAN, -NAN, INFINITY, -INFINITY, 0.5, -0.5, 1.5, 0.05, 0.0005, 0.9999999999,
      -0.9999999999, 1e-10, -1e-10, double(INT_MAX), -double(INT_MAX), double(INT_MAX) - 1, double(INT_MAX) - 0.5,
      -double(INT_MAX) + 0.5, nextafter(double(INT_MAX), 0.0), -nextafter(double(INT_MAX), 0.0), double(INT_MIN),
      2147483646.9999999, 1e300, -1e300, 5e-324, std::numeric_limits<double>::max(), 123.4565, 0.0015, 2.675, 1.005};
  for(double value : specials) { compareAllPrec(value); compareAllPrec(float(value)); }
  // every prec, including out-of-range ones that fall back to stbsp_sprintf
  for(int prec = 10; prec < 16; ++prec) { compareOne(1.25, prec); compareOne(-3.0, prec); }
  // all small integers and all 1/1000 steps in [-100, 100]
  for(int ii = -100000; ii <= 100000; ++ii) { compareAllPrec(ii/1000.0); compareAllPrec(float(ii/1000.0)); compareAllPrec(double(ii)); }

  long nvalues = millions*1000000/10;
  for(long ii = 0; ii < nvalues; ++ii) {
    double value = randomValue();
    compareAllPrec(value);
    if(ii % 4 == 0) compareAllPrec(float(value));
  }
  printf("realtostrtest: %ld comparisons, %ld mismatches\n", nCompared, nMismatch);
  return nMismatch > 0 ? 1 : 0;
}
