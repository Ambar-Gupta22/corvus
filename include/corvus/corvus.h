#pragma once

// corvus — an in-process, offline-capable AI agent runtime for C++.
// Single umbrella header: include this and you have the whole public API.

#include "corvus/agent.h"
#include "corvus/agent_builder.h"
#include "corvus/llm_client.h"
#include "corvus/memory.h"
#include "corvus/mock_llm.h"
#include "corvus/schema.h"
#include "corvus/strategy.h"
#include "corvus/tool.h"
#include "corvus/tool_registry.h"

#define CORVUS_VERSION_MAJOR 0
#define CORVUS_VERSION_MINOR 0
#define CORVUS_VERSION_PATCH 1
