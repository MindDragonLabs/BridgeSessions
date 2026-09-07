#pragma once
// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
//
// bs-upgrade-safety.h — gates for the `bs upgrade` command.
//
// The 2026-09-07 fleet incident: `bridgesessions upgrade` running inside a
// hosted mesh session (BS_SESSION=1 set by bs-pty.h create_session) called
// pause_mesh_daemon(), which SIGTERMed the very daemon carrying the session
// worker's IPC. The upgrade process died before resume_mesh_daemon() ran,
// leaving the systemd unit masked with no auto-restart. That peer stayed
// offline until manual `systemctl --user unmask && start`.
//
// The check is the same for local and remote upgrade attempts — both flow
// through a session-worker, both set BS_SESSION=1. Override at your own risk
// with BS_UPGRADE_IN_MESH=1.
//
// This header does NOT open its own namespace — its caller (bs-protocol.h's
// facade, included as `namespace bs::mesh { #include ... }`) provides it.
// Defining inline functions inside an extra namespace block here would nest
// them as bs::mesh::bs::mesh::name and break lookup from both main.cpp and
// tests.

#include <cstdlib>
#include <string>

inline bool upgrade_in_mesh_session() {
    const char* s = std::getenv("BS_SESSION");
    return s != nullptr && *s != '\0';
}

inline bool upgrade_in_mesh_override() {
    const char* s = std::getenv("BS_UPGRADE_IN_MESH");
    return s != nullptr && std::string(s) == "1";
}
