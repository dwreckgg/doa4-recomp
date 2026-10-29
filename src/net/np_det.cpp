// DOA4 netplay phase 0: the determinism layer. See np_det.h.

#include "np_det.h"

#include "np_session.h"

namespace doa4::np::det {

void Start(uint64_t seed, const std::filesystem::path& dir) {
  (void)seed;
  (void)dir;
}

void OnFrame(uint32_t frame, uint8_t* base) {
  (void)frame;
  (void)base;
}

}  // namespace doa4::np::det
