// Prints the reference values that tests/tools/test_np_tools.py pins for tools/np_bundle.py's pure-Python XXH3
// (the 64-bit hash with a seed that src/net/np_session.cpp BuildId() uses): the library's own answers, from the
// xxhash.h the netplay build compiles against (sdk-np0 thirdparty/xxHash). Not part of any build; run by hand:
//
//   clang-cl /nologo /O2 /I<sdk-np0>\thirdparty\xxHash xxh3_vectors.cpp /Fe:xxh3_vectors.exe
//   xxh3_vectors.exe
//
// Output lines: "vec <length> <seed> <hash>" for the test pattern below (the same one the test generates),
// and "build <exe length> <dll length> <id>" for BuildId()'s composition over two pattern files:
// h = XXH3_64bits_withSeed(exe, 0); h = XXH3_64bits_withSeed(dll, h).
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdint>
#include <cstdio>
#include <vector>

static std::vector<unsigned char> Pattern(size_t n, unsigned mul, unsigned add) {
  std::vector<unsigned char> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = (unsigned char)((i * mul + add) & 0xFF);
  return v;
}

int main() {
  const size_t lengths[] = {0,   1,   2,   3,   4,   5,   8,    9,    16,   17,   32,   33,   64,   65,   96,   97,
                            128, 129, 200, 240, 241, 255, 256,  257,  511,  1023, 1024, 1025, 2047, 2048, 2049, 4101,
                            10000};
  const unsigned long long seeds[] = {0ull, 1ull, 0x9E3779B97F4A7C15ull, 0xFFFFFFFFFFFFFFFFull};
  for (size_t n : lengths) {
    const std::vector<unsigned char> data = Pattern(n, 131, 7);
    for (unsigned long long seed : seeds) {
      std::printf("vec %zu %llu %016llx\n", n, seed,
                  (unsigned long long)XXH3_64bits_withSeed(data.data(), n, (XXH64_hash_t)seed));
    }
  }
  const std::vector<unsigned char> exe = Pattern(300, 17, 1), dll = Pattern(1300, 29, 5);
  uint64_t h = XXH3_64bits_withSeed(exe.data(), exe.size(), 0);
  h = XXH3_64bits_withSeed(dll.data(), dll.size(), h);
  std::printf("build %zu %zu %016llx\n", exe.size(), dll.size(), (unsigned long long)h);
  return 0;
}
