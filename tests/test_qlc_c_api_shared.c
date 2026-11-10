// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License

#include "exports/qlc_diag_logger.h"

#include <assert.h>

int main(void) {
    assert(qlc_abi_version() == QLC_C_ABI_VERSION);
    return 0;
}
