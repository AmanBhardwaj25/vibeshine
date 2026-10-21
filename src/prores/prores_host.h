/**
 * @file src/prores/prores_host.h
 * @brief Platform-neutral entry points of the ProRes host encoder.
 *
 * ProRes rides the PyroWave session loop and transport: a session whose client
 * negotiated bitStreamFormat 4 is encoded here and every frame is intra-coded.
 * The encoder implements pyrowave::host::encoder_t. See docs/prores-protocol.md.
 */
#pragma once

#include "src/pyrowave_host.h"

namespace prores::host {
  /**
   * @brief Create the platform encoder for a session.
   * @return nullptr when ProRes encoding is unavailable on this platform or build.
   */
  std::unique_ptr<pyrowave::host::encoder_t> make_encoder(const pyrowave::host::session_params_t &params, std::shared_ptr<platf::display_t> display);

  /**
   * @brief Whether this build can encode ProRes on the given capture adapter.
   *
   * Creates (and destroys) a GPU device, so call it from encoder probing, not per frame.
   * @param detail Receives the reason when unsupported.
   */
  bool probe(const std::optional<platf::adapter_id_t> &adapter, std::string &detail);
}  // namespace prores::host
