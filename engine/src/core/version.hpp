#pragma once

namespace proj {

#ifndef PROJ_VERSION
#define PROJ_VERSION "0.0.0-dev"
#endif

inline constexpr const char* kToolVersion = PROJ_VERSION;
// Version of the JSON request/response envelope shared with the CLI.
inline constexpr int kProtocolVersion = 1;
// Version of the per-project index database layout.
// v2: raw (pre-resolution) columns and generated-row flags so `proj refresh`
//     can re-resolve relationships in place; call-chain links; return types.
inline constexpr int kIndexSchemaVersion = 2;
// Version of the shared registry database layout.
inline constexpr int kRegistrySchemaVersion = 1;

}  // namespace proj
