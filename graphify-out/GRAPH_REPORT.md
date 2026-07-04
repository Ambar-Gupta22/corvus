# Graph Report - .  (2026-07-04)

## Corpus Check
- 52 files · ~66,877 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 261 nodes · 360 edges · 21 communities (20 shown, 1 thin omitted)
- Extraction: 89% EXTRACTED · 10% INFERRED · 0% AMBIGUOUS · INFERRED: 37 edges (avg confidence: 0.85)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_LLM Client Interface|LLM Client Interface]]
- [[_COMMUNITY_Design Vision & Roadmap|Design Vision & Roadmap]]
- [[_COMMUNITY_Agent Loop & Async|Agent Loop & Async]]
- [[_COMMUNITY_Conversation Memory|Conversation Memory]]
- [[_COMMUNITY_Tooling & MCP Concepts|Tooling & MCP Concepts]]
- [[_COMMUNITY_Tool Abstraction|Tool Abstraction]]
- [[_COMMUNITY_Quickstart & Builder|Quickstart & Builder]]
- [[_COMMUNITY_Schema Builder|Schema Builder]]
- [[_COMMUNITY_Agent Construction|Agent Construction]]
- [[_COMMUNITY_Tool Registry Impl|Tool Registry Impl]]
- [[_COMMUNITY_Tool Registry Core|Tool Registry Core]]
- [[_COMMUNITY_Brainstorm WebSocket Client|Brainstorm WebSocket Client]]
- [[_COMMUNITY_Server Stop Script|Server Stop Script]]
- [[_COMMUNITY_Backend Client Stubs|Backend Client Stubs]]
- [[_COMMUNITY_Agent Constructor|Agent Constructor]]
- [[_COMMUNITY_Brainstorming Skill|Brainstorming Skill]]
- [[_COMMUNITY_Multi-Agent Orchestration|Multi-Agent Orchestration]]
- [[_COMMUNITY_Server Start Script|Server Start Script]]

## God Nodes (most connected - your core abstractions)
1. `Agent` - 17 edges
2. `AgentBuilder` - 15 edges
3. `ToolRegistry` - 12 edges
4. `FunctionTool` - 11 edges
5. `Agent loop` - 11 edges
6. `AgentCallbacks` - 10 edges
7. `InMemoryMemory` - 10 edges
8. `Message` - 9 edges
9. `corvus (C++ AI agent runtime)` - 9 edges
10. `CancelToken` - 8 edges

## Surprising Connections (you probably didn't know these)
- `graphify commit hook + CLAUDE.md integration` --references--> `corvus (C++ AI agent runtime)`  [AMBIGUOUS]
  .claude/skills/graphify/references/hooks.md → CLAUDE.md
- `graphify exports (Neo4j/FalkorDB/MCP/wiki)` --semantically_similar_to--> `MCP-native client`  [INFERRED] [semantically similar]
  .claude/skills/graphify/references/exports.md → CLAUDE.md
- `corvus 12-line quickstart` --references--> `Tool / FunctionTool / makeTool`  [INFERRED]
  README.md → CLAUDE.md
- `claude-mem cross-session memory` --semantically_similar_to--> `Memory / InMemoryMemory`  [INFERRED] [semantically similar]
  .agents/rules/claude-mem-context.md → CLAUDE.md
- `ReAct reasoning+acting loop` --semantically_similar_to--> `Strategy (reasoning)`  [INFERRED] [semantically similar]
  ai-agent orchestration architecture.html → CLAUDE.md

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **corvus core runtime units (the agent loop)** — claude_agent, claude_tool, claude_tool_registry, claude_memory, claude_llm_client, claude_strategy, claude_agent_builder [EXTRACTED 1.00]
- **graphify build/query pipeline** — claude_skills_graphify_skill, claude_skills_graphify_references_extraction_spec, claude_skills_graphify_references_query, claude_skills_graphify_references_update [EXTRACTED 1.00]
- **brainstorming idea-to-design flow** — agents_skills_brainstorming_skill, agents_skills_brainstorming_visual_companion, agents_skills_brainstorming_spec_reviewer, agents_skills_brainstorming_scripts_frame_template [EXTRACTED 1.00]

## Communities (21 total, 1 thin omitted)

### Community 0 - "LLM Client Interface"
Cohesion: 0.09
Nodes (25): deque, onToken, string, vector, LLMClient, complete, name, LLMResponse (+17 more)

### Community 1 - "Design Vision & Roadmap"
Cohesion: 0.10
Nodes (27): claude-mem cross-session memory, AI Agent Orchestration Framework (original vision), Jarvis demo assistant (voice/phone/cloud), The Python wall / LangChain gap, llama.cpp integration, ReAct reasoning+acting loop, Agent loop, AgentBuilder (+19 more)

### Community 2 - "Agent Loop & Async"
Cohesion: 0.13
Nodes (20): atomic, function, future, runImpl, AgentCallbacks, onStep, onToolCall, onToolResult (+12 more)

### Community 3 - "Conversation Memory"
Cohesion: 0.11
Nodes (20): mutex, string, vector, InMemoryMemory, append, clear, context, messages_ (+12 more)

### Community 4 - "Tooling & MCP Concepts"
Cohesion: 0.12
Nodes (20): Plugin system (.so community tools), Six building blocks of orchestration, MCP-native client, Schema builder, graphify add URL / watch folder, graphify exports (Neo4j/FalkorDB/MCP/wiki), graphify extraction subagent spec, graphify GitHub clone + cross-repo merge (+12 more)

### Community 5 - "Tool Abstraction"
Cohesion: 0.17
Nodes (11): Fn, FunctionTool, fn_, schema_, string, ToolPtr, makeTool(), Tool (+3 more)

### Community 6 - "Quickstart & Builder"
Cohesion: 0.12
Nodes (15): main(), AgentBuilder, build, llm_, maxIterations_, memory_, registry_, strategy_ (+7 more)

### Community 7 - "Schema Builder"
Cohesion: 0.15
Nodes (11): Field, string, vector, Schema, fields_, json, string, escape() (+3 more)

### Community 8 - "Agent Construction"
Cohesion: 0.14
Nodes (15): Agent, llm_, maxIterations_, memory_, registry_, run, runAsync, strategy_ (+7 more)

### Community 9 - "Tool Registry Impl"
Cohesion: 0.19
Nodes (12): size_t, string, ToolPtr, vector, ToolRegistry::all(), ToolRegistry::get(), ToolRegistry::has(), ToolRegistry::registerTool() (+4 more)

### Community 10 - "Tool Registry Core"
Cohesion: 0.17
Nodes (12): mutex, string, ToolPtr, ToolRegistry, all, get, has, mutex_ (+4 more)

### Community 11 - "Brainstorm WebSocket Client"
Cohesion: 0.33
Nodes (5): connect(), reloadAfterRecovery(), sessionKey(), setStatus(), websocketUrl()

### Community 12 - "Server Stop Script"
Cohesion: 0.43
Nodes (4): command_has_server_id(), is_brainstorm_server(), mark_stopped(), stop-server.sh script

### Community 13 - "Backend Client Stubs"
Cohesion: 0.60
Nodes (5): anthropic(), LLMClientPtr, string, ollama(), openai()

### Community 14 - "Agent Constructor"
Cohesion: 0.40
Nodes (5): Agent::Agent(), LLMClientPtr, MemoryPtr, Strategy, ToolRegistryPtr

### Community 15 - "Brainstorming Skill"
Cohesion: 0.50
Nodes (4): Brainstorm frame template (CSS), brainstorming skill (idea -> design), Spec document reviewer prompt, Visual Companion (browser brainstorm)

### Community 16 - "Multi-Agent Orchestration"
Cohesion: 0.50
Nodes (4): EventBus / Observer Pattern, Orchestrator + Chain of Responsibility, EventBus (agent-to-agent), Orchestrator (multi-agent)

## Ambiguous Edges - Review These
- `corvus (C++ AI agent runtime)` → `graphify commit hook + CLAUDE.md integration`  [AMBIGUOUS]
  .claude/skills/graphify/references/hooks.md · relation: references

## Knowledge Gaps
- **74 isolated node(s):** `flag_`, `onToolCall`, `onToolResult`, `onStep`, `output` (+69 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **1 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `corvus (C++ AI agent runtime)` and `graphify commit hook + CLAUDE.md integration`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **Why does `Agent` connect `Agent Construction` to `Agent Loop & Async`, `Quickstart & Builder`?**
  _High betweenness centrality (0.092) - this node is a cross-community bridge._
- **Why does `ToolRegistry` connect `Tool Registry Core` to `Schema Builder`?**
  _High betweenness centrality (0.053) - this node is a cross-community bridge._
- **Are the 2 inferred relationships involving `Agent loop` (e.g. with `AgentCallbacks (streaming + observability)` and `Agent loop mental model`) actually correct?**
  _`Agent loop` has 2 INFERRED edges - model-reasoned connections that need verification._
- **What connects `flag_`, `onToolCall`, `onToolResult` to the rest of the system?**
  _75 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `LLM Client Interface` be split into smaller, more focused modules?**
  _Cohesion score 0.08505747126436781 - nodes in this community are weakly interconnected._
- **Should `Design Vision & Roadmap` be split into smaller, more focused modules?**
  _Cohesion score 0.09686609686609686 - nodes in this community are weakly interconnected._