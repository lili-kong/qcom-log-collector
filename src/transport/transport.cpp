// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
//
// Transport base. Only the shared state lives here — the TX/RX worker
// threads, byte queues, lifecycle worker and request/response pairing all
// moved to Protocol, which owns a Transport and drives it.

#include "transport.h"

namespace QC {

Transport::Transport() = default;

Transport::~Transport() = default;

} // namespace QC
