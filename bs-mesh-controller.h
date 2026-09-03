// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-mesh-controller.h — Mesh controller facade (R6b)
// Extracted from bs-protocol.h (R6); class body split 2026-09-03.
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace — parent file provides it.
#pragma once

#include "bs-mesh-support.h"

class MeshController {
#include "bs-mesh-conn.h"
#include "bs-mesh-transfer.h"
#include "bs-mesh-cli.h"
};

#include "bs-mesh-ui.h"
