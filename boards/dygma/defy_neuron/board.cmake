# SPDX-License-Identifier: MIT

# west flash on the zmk variant copies zmk.uf2 onto the UF2 loader's drive
# (docs/uf2-loader.md). The loader itself is written through n2_bootloader, by
# the installer.
if(CONFIG_BOARD_DEFY_NEURON_NRF52833_ZMK)
  board_runner_args(uf2 "--board-id=nRF52833-DefyNeuron-v1")
  include(${ZEPHYR_BASE}/boards/common/uf2.board.cmake)
endif()
