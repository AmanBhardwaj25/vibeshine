/**
 * @file src/prores/prores_host.cpp
 * @brief ProRes host encoder for builds and platforms without an implementation.
 *
 * Windows builds with SUNSHINE_ENABLE_PRORES use src/platform/windows/prores_encode.cpp instead.
 */
#include "prores_host.h"

#if !(defined(_WIN32) && defined(SUNSHINE_ENABLE_PRORES))

  #include "platform/common.h"

namespace prores::host {
  std::unique_ptr<pyrowave::host::encoder_t> make_encoder(const pyrowave::host::session_params_t &, std::shared_ptr<platf::display_t>) {
    return nullptr;
  }

  bool probe(const std::optional<platf::adapter_id_t> &, std::string &detail) {
    detail = "ProRes encoding is only implemented on Windows";
    return false;
  }
}  // namespace prores::host

#endif
