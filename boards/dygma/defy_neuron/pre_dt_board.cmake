# SPDX-License-Identifier: MIT

# nRF52 nodes share base addresses: clock and power at 0x40000000, acl and the
# flash controller at 0x4001E000.
list(APPEND EXTRA_DTC_FLAGS "-Wno-unique_unit_address_if_enabled")
